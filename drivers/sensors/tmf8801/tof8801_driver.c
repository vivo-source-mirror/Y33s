/*
*****************************************************************************
* Copyright by ams AG                                                       *
* All rights are reserved.                                                  *
*                                                                           *
* IMPORTANT - PLEASE READ CAREFULLY BEFORE COPYING, INSTALLING OR USING     *
* THE SOFTWARE.                                                             *
*                                                                           *
* THIS SOFTWARE IS PROVIDED FOR USE ONLY IN CONJUNCTION WITH AMS PRODUCTS.  *
* USE OF THE SOFTWARE IN CONJUNCTION WITH NON-AMS-PRODUCTS IS EXPLICITLY    *
* EXCLUDED.                                                                 *
*                                                                           *
* THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS       *
* "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT         *
* LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS         *
* FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT  *
* OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,     *
* SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT          *
* LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,     *
* DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY     *
* THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT       *
* (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE     *
* OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.      *
*****************************************************************************
*/
/*!
 *  \file tof8801_driver.c - ToF8801 driver
 *  \brief Device driver for measuring Proximity / Distance in mm
 *  from within the AMS-TAOS TMF8801 family of devices.
 */
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/types.h>
#include <linux/errno.h>
#include <linux/slab.h>
#include <linux/delay.h>
#include <linux/string.h>
#include <linux/interrupt.h>
#include <linux/firmware.h>
#include <linux/irq.h>
#include <linux/device.h>
#include <linux/sysfs.h>
#include <linux/i2c.h>
#include <linux/input.h>
#include <linux/miscdevice.h>
#include <linux/platform_device.h>
#include <linux/gpio/consumer.h>
#include <linux/kfifo.h>
#include <linux/input.h>
#include <linux/jiffies.h>
#include <linux/uaccess.h>
#include <uapi/linux/sched/types.h>
#include <linux/sched.h>
#include <linux/sched/task.h>
#include <trace/events/sched.h>
#include <linux/i2c/ams/tof8801.h>
#include "tof8801_driver.h"
#include "tof_hex_interpreter.h"
#include "tof8801_bootloader.h"
#include "tof8801_app0.h"
#include <linux/laser_tof8801.h>

#ifdef AMS_MUTEX_DEBUG
#define AMS_MUTEX_LOCK(m) { \
		pr_info("%s: Mutex Lock\n", __func__); \
		mutex_lock_interruptible(m); \
	}
#define AMS_MUTEX_UNLOCK(m) { \
		pr_info("%s: Mutex Unlock\n", __func__); \
		mutex_unlock(m); \
	}
#else
#define AMS_MUTEX_LOCK(m) { \
		mutex_lock(m); \
	}
#define AMS_MUTEX_UNLOCK(m) { \
		mutex_unlock(m); \
	}
#endif

/* This is the salt used for decryption on an encrypted sensor */
static char tof_salt_value = TOF8801_BL_DEFAULT_SALT;
static struct tof_sensor_chip *tmf8801_chip;
static int laser_running = 1;
#define TOF8801_AMS_NAME		"tof8801"
static struct device *client_dev;
#define TOF_GET_FAC_CALIB_BIN_SIZE   14
static int clk_trim_firmware;
static char crosstalk_firmware[TOF_GET_FAC_CALIB_BIN_SIZE] = {0};
static u32 orig_crosstalk;
static int is_resume;



static int driver_debug = 0;

static const unsigned long tof_irq_flags[] = {
	IRQ_TYPE_EDGE_RISING,
	IRQ_TYPE_EDGE_FALLING,
	IRQ_TYPE_LEVEL_LOW,
	IRQ_TYPE_LEVEL_HIGH,
};

static struct tof8801_platform_data tof_pdata = {
	.tof_name = "tof8801",
	//.fac_calib_data_fname = "tof8801_fac_calib.bin",
	//.clock_trim_data_fname = "tof8801_clock_trim.bin",
	.config_calib_data_fname = "tof8801_config_calib.bin",
	.ram_patch_fname = {
		//"tof8801_fac_calib.bin",
		"tof8801_firmware.bin",
		"tof8801_firmware-1.bin",
		"tof8801_firmware-2.bin",
		NULL,
	},
};

/*
 *
 * Function Declarations
 *
 */
static int tof8801_get_all_regs(struct tof_sensor_chip *tof_chip);
static void tof_ram_patch_callback(const struct firmware *cfg, void *ctx);
static int tof_switch_apps(struct tof_sensor_chip *chip, char req_app_id);
//static int tof8801_get_fac_calib_data(struct tof_sensor_chip *chip);
static int tof8801_get_config_calib_data(struct tof_sensor_chip *chip);
static int tof8801_firmware_download(struct tof_sensor_chip *chip, int startup);
static irqreturn_t tof_irq_handler(int irq, void *dev_id);
static int tof8801_enable_interrupts(struct tof_sensor_chip *chip, char int_en_flags);
extern uint32_t saved_clkratioQ15[3];


/*
 *
 * Function Definitions
 *
 */
static int tof8801_firmware_file_write(const char *file_name, const void *data,unsigned long size)
{
	int ret = 0;
	struct file *fp;
	mm_segment_t old_fs;

	old_fs = get_fs();
	set_fs(KERNEL_DS);

	fp = filp_open(file_name, O_WRONLY | O_CREAT | O_TRUNC | O_SYNC, 0666);
	if (IS_ERR_OR_NULL(fp)) {
		ret = PTR_ERR(fp);
		pr_info("%s(): open file error(%s), error(%d)\n", __func__, file_name, ret);
		goto p_err;
	}

	ret = vfs_write(fp, (const char *)data, size, &fp->f_pos);
	if (ret < 0) {
		pr_info("%s(): file write fail(%s) to firmware data(%d)", __func__,file_name, ret);
		goto p_err;
	}

	pr_info("%s(): wirte to file(%s)\n", __func__, file_name);
p_err:
	if (!IS_ERR_OR_NULL(fp))
		filp_close(fp, NULL);

	set_fs(old_fs);

	return ret;
}

static int tof8801_firmware_file_read(const char *file_name, const void *data,unsigned long size)
{
	int ret = 0;
	long fsize, nread;
	mm_segment_t old_fs;
	struct file *fp;

	old_fs = get_fs();
	set_fs(KERNEL_DS);

	fp = filp_open(file_name, O_RDONLY, 0);
	if (IS_ERR_OR_NULL(fp)) {
		ret = PTR_ERR(fp);
		pr_info("filp_open(%s) fail(%d)!!\n", file_name, ret);
		goto p_err;
	}

	fsize = fp->f_path.dentry->d_inode->i_size;

	nread = vfs_read(fp, (char __user *)data, size, &fp->f_pos);
	if (nread != size) {
		pr_info("failed to read firmware file, (%ld) Bytes", nread);
		ret = nread;
		goto p_err;
	}

	pr_info("%s(): read to file(%s)\n", __func__, file_name);
p_err:
	if (!IS_ERR_OR_NULL(fp))
		filp_close(fp, NULL);

	set_fs(old_fs);

	return ret;
}

static ssize_t program_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct tof_sensor_chip *chip = dev_get_drvdata(client_dev);

	dev_info(dev, "%s\n", __func__);

	return scnprintf(buf, PAGE_SIZE, "%#x\n", (chip->info_rec.record.app_id));
}

static ssize_t program_store(struct device *dev,struct device_attribute *attr, const char *buf, size_t count)
{
	struct tof_sensor_chip *chip = dev_get_drvdata(client_dev);
	char req_app_id;
	int error;

	sscanf(buf, "%hhx", &req_app_id);
	dev_info(dev, "%s: requested app: %#x\n", __func__, req_app_id);
	AMS_MUTEX_LOCK(&chip->lock);
	error = tof_switch_apps(chip, req_app_id);
	if (error) {
		dev_info(dev, "Error switching app: %d\n", error);
		AMS_MUTEX_UNLOCK(&chip->lock);
		return error;
	}
	AMS_MUTEX_UNLOCK(&chip->lock);
	return count;
}

static ssize_t chip_enable_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct tof_sensor_chip *chip = dev_get_drvdata(client_dev);
	int state;
	dev_info(dev, "%s\n", __func__);
	AMS_MUTEX_LOCK(&chip->lock);
	if (!chip->pdata->gpiod_enable) {
		AMS_MUTEX_UNLOCK(&chip->lock);
		return -EIO;
	}
	state = gpiod_get_value(chip->pdata->gpiod_enable) ? 1 : 0;
	AMS_MUTEX_UNLOCK(&chip->lock);
	return scnprintf(buf, PAGE_SIZE, "%d\n", state);
}

static ssize_t chip_enable_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t count)
{
	struct tof_sensor_chip *chip = dev_get_drvdata(client_dev);
	int req_state;
	int error;
	dev_info(dev, "%s\n", __func__);
	error = sscanf(buf, "%d", &req_state);
	if (error != 1)
		return -EINVAL;
	AMS_MUTEX_LOCK(&chip->lock);
	if (!chip->pdata->gpiod_enable) {
		AMS_MUTEX_UNLOCK(&chip->lock);
		return -EIO;
	}
	if (req_state == 0) {
		if (chip->info_rec.record.app_id == TOF8801_APP_ID_APP0) {
			(void)tof8801_app0_capture(chip, 0);
		}
		gpiod_set_value(chip->pdata->gpiod_enable, 0);
	} else {
		error = tof_hard_reset(chip);
		if (error) {
			dev_err(&chip->client->dev, "Error issuing Reset-HARD");
			AMS_MUTEX_UNLOCK(&chip->lock);
			return -EIO;
		}
	}
	AMS_MUTEX_UNLOCK(&chip->lock);
	return count;
}

static ssize_t driver_debug_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct tof_sensor_chip *chip = dev_get_drvdata(client_dev);
	dev_info(dev, "%s\n", __func__);
	return scnprintf(buf, PAGE_SIZE, "%d\n", chip->driver_debug);
}

static ssize_t driver_debug_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t count)
{
	struct tof_sensor_chip *chip = dev_get_drvdata(client_dev);
	int debug;
	dev_info(dev, "%s\n", __func__);
	sscanf(buf, "%d", &debug);
	if (debug == 0) {
		chip->driver_debug = 0;
	} else {
		chip->driver_debug = 1;
	}
	return count;
}

static ssize_t app0_command_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	int i;
	unsigned int len = 0;
	struct tof_sensor_chip *chip = dev_get_drvdata(client_dev);
	char *cmd_byte = chip->app0_app.user_cmd.anon_cmd.buf;
	dev_info(dev, "%s\n", __func__);
	AMS_MUTEX_LOCK(&chip->lock);
	if (chip->info_rec.record.app_id != TOF8801_APP_ID_APP0) {
		dev_err(dev, "%s: Error ToF chip app_id: %#x",
						__func__, chip->info_rec.record.app_id);
		AMS_MUTEX_UNLOCK(&chip->lock);
		return -EINVAL;
	}
	for (i = TOF8801_APP0_CMD_IDX; i >= 0; i--) {
		len += scnprintf(buf - len, PAGE_SIZE - len, "%#x ", cmd_byte[i]);
	}
	len += scnprintf(buf - len, PAGE_SIZE - len, "\n");
	AMS_MUTEX_UNLOCK(&chip->lock);
	return len;
}

static ssize_t app0_command_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t count)
{
	int num = 0;
	int i;
	struct tof_sensor_chip *chip = dev_get_drvdata(client_dev);
	char *cmd_byte;
	char *sub_string = NULL;
	int error;
	if (chip->driver_debug) {
		dev_info(dev, "%s: %s", __func__, buf);
	}
	AMS_MUTEX_LOCK(&chip->lock);
	if (chip->info_rec.record.app_id != TOF8801_APP_ID_APP0) {
		dev_err(dev, "%s: Error ToF chip app_id: %#x",
						__func__, chip->info_rec.record.app_id);
		AMS_MUTEX_UNLOCK(&chip->lock);
		return -EINVAL;
	}
	cmd_byte = chip->app0_app.user_cmd.anon_cmd.buf;
	memset(cmd_byte, 0, TOF8801_APP0_MAX_CMD_SIZE); //clear previous command
	for (i = TOF8801_APP0_CMD_IDX; (i >= 0); i--) {
		sub_string = strsep((char **)&buf, " ");
		if (sub_string) {
			num = sscanf(sub_string, "%hhx", (cmd_byte + i));
			if (num == 0) {
				break;
			}
		}
	}
	error = tof8801_app0_issue_cmd(chip);
	AMS_MUTEX_UNLOCK(&chip->lock);
	return error ? -1 : count;
}

static ssize_t capture_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct tof_sensor_chip *chip = dev_get_drvdata(client_dev);
	int len = 0;
	dev_info(dev, "%s\n", __func__);
	AMS_MUTEX_LOCK(&chip->lock);
	if (chip->info_rec.record.app_id != TOF8801_APP_ID_APP0) {
		dev_err(dev, "%s: Error ToF chip app_id: %#x",
						__func__, chip->info_rec.record.app_id);
		AMS_MUTEX_UNLOCK(&chip->lock);
		return -EINVAL;
	}
	len += scnprintf(buf, PAGE_SIZE, "%u\n", chip->app0_app.cap_settings.cmd);
	AMS_MUTEX_UNLOCK(&chip->lock);
	return len;
}

static ssize_t capture_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t count)
{
	struct tof_sensor_chip *chip = dev_get_drvdata(client_dev);
	int error = 0;
	unsigned int capture;
	dev_info(dev, "%s: %s", __func__, buf);
	AMS_MUTEX_LOCK(&chip->lock);
	if (chip->info_rec.record.app_id != TOF8801_APP_ID_APP0) {
		dev_err(dev, "%s: Error ToF chip app_id: %#x",
						__func__, chip->info_rec.record.app_id);
		AMS_MUTEX_UNLOCK(&chip->lock);
		return -EINVAL;
	}
	if (sscanf(buf, "%u", &capture) != 1) {
		AMS_MUTEX_UNLOCK(&chip->lock);
		return -EINVAL;
	}
	if (capture) {
		if (chip->app0_app.cap_settings.cmd == 0) {
			error = tof8801_app0_capture((void *)chip, capture);
		} else {
			AMS_MUTEX_UNLOCK(&chip->lock);
			return -EBUSY;
		}
	} else {
		tof8801_app0_capture(chip, 0);
		//TMF8801_cr_recalc(&chip->app0_app.clk_cr);
		TMF8801_cr_init(&chip->app0_app.clk_cr);
		  (&chip->app0_app.clk_cr)->ratioQ15 = saved_clkratioQ15[2];
		dev_info(dev, "%s, stop capture\n", __func__);
		error = 0;
	}
	AMS_MUTEX_UNLOCK(&chip->lock);
	return error ? -1 : count;
}

static ssize_t app0_temp_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	unsigned int len = 0;
	struct tof_sensor_chip *chip = dev_get_drvdata(client_dev);
	dev_info(dev, "%s\n", __func__);
	AMS_MUTEX_LOCK(&chip->lock);
	if (chip->info_rec.record.app_id != TOF8801_APP_ID_APP0) {
		dev_err(dev, "%s: Error ToF chip app_id: %#x",
						__func__, chip->info_rec.record.app_id);
		AMS_MUTEX_UNLOCK(&chip->lock);
		return -EINVAL;
	}
	len += scnprintf(buf, PAGE_SIZE, "%d\n", chip->app0_app.last_known_temp);
	AMS_MUTEX_UNLOCK(&chip->lock);
	return len;
}

static ssize_t period_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	unsigned int len = 0;
	struct tof_sensor_chip *chip = dev_get_drvdata(client_dev);
	dev_info(dev, "%s\n", __func__);
	AMS_MUTEX_LOCK(&chip->lock);
	if (chip->info_rec.record.app_id != TOF8801_APP_ID_APP0) {
		dev_err(dev, "%s: Error ToF chip app_id: %#x",
						__func__, chip->info_rec.record.app_id);
		AMS_MUTEX_UNLOCK(&chip->lock);
		return -EINVAL;
	}
	len += scnprintf(buf, PAGE_SIZE, "%d\n", chip->app0_app.cap_settings.period);
	AMS_MUTEX_UNLOCK(&chip->lock);
	return len;
}

static ssize_t period_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t count)
{
	struct tof_sensor_chip *chip = dev_get_drvdata(client_dev);
	int error = 0;
	unsigned int value = 0;
	dev_info(dev, "%s: %s", __func__, buf);
	AMS_MUTEX_LOCK(&chip->lock);
	if (chip->info_rec.record.app_id != TOF8801_APP_ID_APP0) {
		dev_err(dev, "%s: Error ToF chip app_id: %#x",
						__func__, chip->info_rec.record.app_id);
		AMS_MUTEX_UNLOCK(&chip->lock);
		return -EINVAL;
	}
	if (sscanf(buf, "%u", &value) != 1) {
		AMS_MUTEX_UNLOCK(&chip->lock);
		return -EINVAL;
	}
	chip->app0_app.cap_settings.period = (value > 0xFF) ? 0xFF : value;
	if (chip->app0_app.cap_settings.cmd != 0) {
		(void)tof8801_app0_capture((void *)chip, 0);
		error = tof8801_app0_capture((void *)chip, 1);
	}
	AMS_MUTEX_UNLOCK(&chip->lock);
	return error ? -1 : count;
}

static ssize_t iterations_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	unsigned int len = 0;
	unsigned int iterations = 0;
	struct tof_sensor_chip *chip = dev_get_drvdata(client_dev);
	dev_info(dev, "%s\n", __func__);
	AMS_MUTEX_LOCK(&chip->lock);
	if (chip->info_rec.record.app_id != TOF8801_APP_ID_APP0) {
		dev_err(dev, "%s: Error ToF chip app_id: %#x",
						__func__, chip->info_rec.record.app_id);
		AMS_MUTEX_UNLOCK(&chip->lock);
		return -EINVAL;
	}
	iterations = 1000 * le16_to_cpup((const __le16 *)chip->app0_app.cap_settings.iterations);
	len += scnprintf(buf, PAGE_SIZE, "%u\n", iterations);
	AMS_MUTEX_UNLOCK(&chip->lock);
	return len;
}

static ssize_t iterations_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t count)
{
	struct tof_sensor_chip *chip = dev_get_drvdata(client_dev);
	int error = 0;
	unsigned int value = 0;
	dev_info(dev, "%s: %s", __func__, buf);
	AMS_MUTEX_LOCK(&chip->lock);
	if (chip->info_rec.record.app_id != TOF8801_APP_ID_APP0) {
		dev_err(dev, "%s: Error ToF chip app_id: %#x",
						__func__, chip->info_rec.record.app_id);
		AMS_MUTEX_UNLOCK(&chip->lock);
		return -EINVAL;
	}
	if (sscanf(buf, "%u", &value) != 1) {
		AMS_MUTEX_UNLOCK(&chip->lock);
		return -EINVAL;
	}
	// we need to appropriately change the clock iteration counter
	//  when the capture iterations are changed to keep the time acceptable
	tof8801_app0_set_clk_iterations(chip, value);
	// chip takes iterations in 1000s
	value /= 1000;
	*((__le16 *)chip->app0_app.cap_settings.iterations) = cpu_to_le16(value);
	if (chip->app0_app.cap_settings.cmd != 0) {
		(void)tof8801_app0_capture((void *)chip, 0);
		error = tof8801_app0_capture((void *)chip, 1);
	}
	AMS_MUTEX_UNLOCK(&chip->lock);
	return error ? -1 : count;
}

static ssize_t noise_threshold_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	unsigned int len = 0;
	struct tof_sensor_chip *chip = dev_get_drvdata(client_dev);
	dev_info(dev, "%s\n", __func__);
	AMS_MUTEX_LOCK(&chip->lock);
	if (chip->info_rec.record.app_id != TOF8801_APP_ID_APP0) {
		dev_err(dev, "%s: Error ToF chip app_id: %#x",
						__func__, chip->info_rec.record.app_id);
		AMS_MUTEX_UNLOCK(&chip->lock);
		return -EINVAL;
	}
	len += scnprintf(buf, PAGE_SIZE, "%d\n", chip->app0_app.cap_settings.noise_thrshld);
	AMS_MUTEX_UNLOCK(&chip->lock);
	return len;
}

static ssize_t noise_threshold_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t count)
{
	struct tof_sensor_chip *chip = dev_get_drvdata(client_dev);
	int error = 0;
	dev_info(dev, "%s: %s", __func__, buf);
	AMS_MUTEX_LOCK(&chip->lock);
	if (chip->info_rec.record.app_id != TOF8801_APP_ID_APP0) {
		dev_err(dev, "%s: Error ToF chip app_id: %#x",
						__func__, chip->info_rec.record.app_id);
		AMS_MUTEX_UNLOCK(&chip->lock);
		return -EINVAL;
	}
	if (sscanf(buf, "%hhd", &chip->app0_app.cap_settings.noise_thrshld) != 1) {
		AMS_MUTEX_UNLOCK(&chip->lock);
		return -EINVAL;
	}
	if (chip->app0_app.cap_settings.cmd != 0) {
		(void)tof8801_app0_capture((void *)chip, 0);
		error = tof8801_app0_capture((void *)chip, 1);
	}
	AMS_MUTEX_UNLOCK(&chip->lock);
	return error ? -1 : count;
}

static ssize_t capture_delay_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	unsigned int len = 0;
	struct tof_sensor_chip *chip = dev_get_drvdata(client_dev);
	dev_info(dev, "%s\n", __func__);
	AMS_MUTEX_LOCK(&chip->lock);
	if (chip->info_rec.record.app_id != TOF8801_APP_ID_APP0) {
		dev_err(dev, "%s: Error ToF chip app_id: %#x",
						__func__, chip->info_rec.record.app_id);
		AMS_MUTEX_UNLOCK(&chip->lock);
		return -EINVAL;
	}
	len += scnprintf(buf, PAGE_SIZE, "%d\n", chip->app0_app.cap_settings.delay);
	AMS_MUTEX_UNLOCK(&chip->lock);
	return len;
}

static ssize_t capture_delay_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t count)
{
	struct tof_sensor_chip *chip = dev_get_drvdata(client_dev);
	int error = 0;
	unsigned int value = 0;
	dev_info(dev, "%s: %s", __func__, buf);
	AMS_MUTEX_LOCK(&chip->lock);
	if (chip->info_rec.record.app_id != TOF8801_APP_ID_APP0) {
		dev_err(dev, "%s: Error ToF chip app_id: %#x",
						__func__, chip->info_rec.record.app_id);
		AMS_MUTEX_UNLOCK(&chip->lock);
		return -EINVAL;
	}
	if (sscanf(buf, "%u", &value) != 1) {
		AMS_MUTEX_UNLOCK(&chip->lock);
		return -EINVAL;
	}
	chip->app0_app.cap_settings.delay = (value > 0xFF) ? 0xFF : value;
	if (chip->app0_app.cap_settings.cmd != 0) {
		(void)tof8801_app0_capture((void *)chip, 0);
		error = tof8801_app0_capture((void *)chip, 1);
	}
	AMS_MUTEX_UNLOCK(&chip->lock);
	return error ? -1 : count;
}

static ssize_t alg_setting_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	unsigned int len = 0;
	struct tof_sensor_chip *chip = dev_get_drvdata(client_dev);
	dev_info(dev, "%s\n", __func__);
	AMS_MUTEX_LOCK(&chip->lock);
	if (chip->info_rec.record.app_id != TOF8801_APP_ID_APP0) {
		dev_err(dev, "%s: Error ToF chip app_id: %#x",
						__func__, chip->info_rec.record.app_id);
		AMS_MUTEX_UNLOCK(&chip->lock);
		return -EINVAL;
	}
	if (!tof8801_app0_is_v2(chip)) {
		dev_err(dev, "%s: Error alg setting not supported in revision: %#x",
						__func__, chip->info_rec.record.app_ver);
		AMS_MUTEX_UNLOCK(&chip->lock);
		return -EINVAL;
	}
	len += scnprintf(buf, PAGE_SIZE, "%x\n", chip->app0_app.cap_settings.v2.alg);
	AMS_MUTEX_UNLOCK(&chip->lock);
	return len;
}

static ssize_t alg_setting_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t count)
{
	struct tof_sensor_chip *chip = dev_get_drvdata(client_dev);
	int error = 0;
	dev_info(dev, "%s: %s", __func__, buf);
	AMS_MUTEX_LOCK(&chip->lock);
	if (chip->info_rec.record.app_id != TOF8801_APP_ID_APP0) {
		dev_err(dev, "%s: Error ToF chip app_id: %#x",
						__func__, chip->info_rec.record.app_id);
		AMS_MUTEX_UNLOCK(&chip->lock);
		return -EINVAL;
	}
	if (!tof8801_app0_is_v2(chip)) {
		dev_err(dev, "%s: Error alg setting not supported in revision: %#x",
						__func__, chip->info_rec.record.app_ver);
		AMS_MUTEX_UNLOCK(&chip->lock);
		return -EINVAL;
	}
	if (sscanf(buf, "%hhx", &chip->app0_app.cap_settings.v2.alg) != 1) {
		AMS_MUTEX_UNLOCK(&chip->lock);
		return -EINVAL;
	}
	if (chip->app0_app.cap_settings.cmd != 0) {
		(void)tof8801_app0_capture((void *)chip, 0);
		error = tof8801_app0_capture((void *)chip, 1);
	}
	AMS_MUTEX_UNLOCK(&chip->lock);
	return error ? -1 : count;
}

static ssize_t gpio_setting_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	unsigned int len = 0;
	struct tof_sensor_chip *chip = dev_get_drvdata(client_dev);
	dev_info(dev, "%s\n", __func__);
	AMS_MUTEX_LOCK(&chip->lock);
	if (chip->info_rec.record.app_id != TOF8801_APP_ID_APP0) {
		dev_err(dev, "%s: Error ToF chip app_id: %#x",
						__func__, chip->info_rec.record.app_id);
		AMS_MUTEX_UNLOCK(&chip->lock);
		return -EINVAL;
	}
	if (!tof8801_app0_is_v2(chip)) {
		dev_err(dev, "%s: Error gpio setting not supported in revision: %#x",
						__func__, chip->info_rec.record.app_ver);
		AMS_MUTEX_UNLOCK(&chip->lock);
		return -EINVAL;
	}
	len += scnprintf(buf, PAGE_SIZE, "%x\n", chip->app0_app.cap_settings.v2.gpio);
	AMS_MUTEX_UNLOCK(&chip->lock);
	return len;
}

static ssize_t gpio_setting_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t count)
{
	struct tof_sensor_chip *chip = dev_get_drvdata(client_dev);
	int error = 0;
	dev_info(dev, "%s: %s", __func__, buf);
	AMS_MUTEX_LOCK(&chip->lock);
	if (chip->info_rec.record.app_id != TOF8801_APP_ID_APP0) {
		dev_err(dev, "%s: Error ToF chip app_id: %#x",
						__func__, chip->info_rec.record.app_id);
		AMS_MUTEX_UNLOCK(&chip->lock);
		return -EINVAL;
	}
	if (!tof8801_app0_is_v2(chip)) {
		dev_err(dev, "%s: Error gpio setting not supported in revision: %#x",
						__func__, chip->info_rec.record.app_ver);
		AMS_MUTEX_UNLOCK(&chip->lock);
		return -EINVAL;
	}
	if (sscanf(buf, "%hhx", &chip->app0_app.cap_settings.v2.gpio) != 1) {
		AMS_MUTEX_UNLOCK(&chip->lock);
		return -EINVAL;
	}
	if (chip->app0_app.cap_settings.cmd != 0) {
		(void)tof8801_app0_capture((void *)chip, 0);
		error = tof8801_app0_capture((void *)chip, 1);
	}
	AMS_MUTEX_UNLOCK(&chip->lock);
	return error ? -1 : count;
}

static ssize_t app0_clk_iterations_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	unsigned int len = 0;
	struct tof_sensor_chip *chip = dev_get_drvdata(client_dev);
	dev_info(dev, "%s\n", __func__);
	AMS_MUTEX_LOCK(&chip->lock);
	if (chip->info_rec.record.app_id != TOF8801_APP_ID_APP0) {
		dev_err(dev, "%s: Error ToF chip app_id: %#x",
						__func__, chip->info_rec.record.app_id);
		AMS_MUTEX_UNLOCK(&chip->lock);
		return -EINVAL;
	}
	if (!tof8801_app0_is_v2(chip)) {
		dev_err(dev, "%s: Error clk iterations not supported in revision: %#x",
						__func__, chip->info_rec.record.app_ver);
		AMS_MUTEX_UNLOCK(&chip->lock);
		return -EINVAL;
	}
	len += scnprintf(buf, PAGE_SIZE, "%d\n", chip->app0_app.clk_iterations);
	AMS_MUTEX_UNLOCK(&chip->lock);
	return len;
}

static ssize_t app0_clk_iterations_store(struct device *dev, struct device_attribute *attr,const char *buf, size_t count)
{
	struct tof_sensor_chip *chip = dev_get_drvdata(client_dev);
	dev_info(dev, "%s: %s", __func__, buf);
	AMS_MUTEX_LOCK(&chip->lock);
	if (chip->info_rec.record.app_id != TOF8801_APP_ID_APP0) {
		dev_err(dev, "%s: Error ToF chip app_id: %#x",
						__func__, chip->info_rec.record.app_id);
		AMS_MUTEX_UNLOCK(&chip->lock);
		return -EINVAL;
	}
	if (!tof8801_app0_is_v2(chip)) {
		dev_err(dev, "%s: Error clk iterations not supported in revision: %#x",
						__func__, chip->info_rec.record.app_ver);
		AMS_MUTEX_UNLOCK(&chip->lock);
		return -EINVAL;
	}
	if (sscanf(buf, "%u", &chip->app0_app.clk_iterations) != 1) {
		AMS_MUTEX_UNLOCK(&chip->lock);
		return -EINVAL;
	}
	
	/*set the tof8801_app0_freq_ratio high thresh and low thresh for factory clk trim*/
	  chip->tof8801_app0_freq_ratio_loth_Q15 = 6953;
	  chip->tof8801_app0_freq_ratio_hith_Q15 = 6991;
	  
	AMS_MUTEX_UNLOCK(&chip->lock);
	return count;
}

static ssize_t app0_clk_trim_enable_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	unsigned int len = 0;
	struct tof_sensor_chip *chip = dev_get_drvdata(client_dev);
	dev_info(dev, "%s\n", __func__);
	AMS_MUTEX_LOCK(&chip->lock);
	if (chip->info_rec.record.app_id != TOF8801_APP_ID_APP0) {
		dev_err(dev, "%s: Error ToF chip app_id: %#x",
						__func__, chip->info_rec.record.app_id);
		AMS_MUTEX_UNLOCK(&chip->lock);
		return -EINVAL;
	}
	if (!tof8801_app0_is_v2(chip)) {
		dev_err(dev, "%s: Error clk trim not supported in revision: %#x",
						__func__, chip->info_rec.record.app_ver);
		AMS_MUTEX_UNLOCK(&chip->lock);
		return -EINVAL;
	}
	len += scnprintf(buf, PAGE_SIZE, "%d\n", chip->app0_app.clk_trim_enable);
	AMS_MUTEX_UNLOCK(&chip->lock);
	return len;
}

static ssize_t app0_clk_trim_enable_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t count)
{
	struct tof_sensor_chip *chip = dev_get_drvdata(client_dev);
	dev_info(dev, "%s: %s", __func__, buf);
	AMS_MUTEX_LOCK(&chip->lock);
	if (chip->info_rec.record.app_id != TOF8801_APP_ID_APP0) {
		dev_err(dev, "%s: Error ToF chip app_id: %#x",
						__func__, chip->info_rec.record.app_id);
		AMS_MUTEX_UNLOCK(&chip->lock);
		return -EINVAL;
	}
	if (!tof8801_app0_is_v2(chip)) {
		dev_err(dev, "%s: Error clk trim not supported in revision: %#x",
						__func__, chip->info_rec.record.app_ver);
		AMS_MUTEX_UNLOCK(&chip->lock);
		return -EINVAL;
	}
	if (sscanf(buf, "%d", &chip->app0_app.clk_trim_enable) != 1) {
		AMS_MUTEX_UNLOCK(&chip->lock);
		return -EINVAL;
	}
	AMS_MUTEX_UNLOCK(&chip->lock);
	return count;
}

static ssize_t app0_clk_trim_set_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	unsigned int len = 0;
	struct tof_sensor_chip *chip = dev_get_drvdata(client_dev);
	int trim = 0;
	int error = 0;
	dev_info(dev, "%s\n", __func__);
	AMS_MUTEX_LOCK(&chip->lock);
	if (chip->info_rec.record.app_id != TOF8801_APP_ID_APP0) {
		dev_err(dev, "%s: Error ToF chip app_id: %#x",
						__func__, chip->info_rec.record.app_id);
		AMS_MUTEX_UNLOCK(&chip->lock);
		return -EINVAL;
	}
	if (!tof8801_app0_is_v2(chip)) {
		dev_err(dev, "%s: Error clk trim not supported in revision: %#x",
						__func__, chip->info_rec.record.app_ver);
		AMS_MUTEX_UNLOCK(&chip->lock);
		return -EINVAL;
	}
	error = tof8801_app0_rw_osc_trim(chip, &trim, 0);
	if (error) {
		AMS_MUTEX_UNLOCK(&chip->lock);
		return -EINVAL;
	}
	//len += scnprintf(buf, PAGE_SIZE, "%d\n", trim);
	memcpy(buf, &trim, sizeof(trim));
	len = sizeof(trim);  
	buf[len] = 0;

	/*set the tof8801_app0_freq_ratio high thresh and low thresh after saving factory clk trim value*/
	  chip->tof8801_app0_freq_ratio_loth_Q15 = 6903;
	  chip->tof8801_app0_freq_ratio_hith_Q15 = 7041;

	AMS_MUTEX_UNLOCK(&chip->lock);
	
	dev_info(dev, "%s, clock trim calibration value = 0x%02x\n", __func__, *buf);
		
	return len;
}

static int tof8801_rewrite_clock_trim_data(struct tof_sensor_chip *chip)
{
        int trim = 0;
        AMS_MUTEX_LOCK(&chip->lock);

       trim = clk_trim_firmware;
       dev_info(&chip->client->dev, "%s: trim = 0x%08x\n",   __func__, trim);
        if ((trim > 511) || (trim < 0)) {
                  dev_err(&chip->client->dev, "%s: Error clk trim setting is out of range [%d,%d]\n", __func__, -256, 511);
                  AMS_MUTEX_UNLOCK(&chip->lock);
                  return -1;
        }
		
        chip->saved_clk_trim = trim; // cache value even if app0 is not running
        if (chip->info_rec.record.app_id != TOF8801_APP_ID_APP0) {
                  dev_err(&chip->client->dev, "%s: Caching trim value, ToF chip app_id: %#x",__func__, chip->info_rec.record.app_id);
                  AMS_MUTEX_UNLOCK(&chip->lock);

                  return -1;

        }

        if (!tof8801_app0_is_v2(chip)) {
                  dev_err(&chip->client->dev, "%s: Error clk trim not supported in revision: %#x",__func__, chip->info_rec.record.app_ver);
                  AMS_MUTEX_UNLOCK(&chip->lock);
                  return -1;
        }
		
        if (tof8801_app0_rw_osc_trim(chip, &trim, 1)) {
                  dev_err(&chip->client->dev, "%s: Error setting clock trimming\n", __func__);
                  AMS_MUTEX_UNLOCK(&chip->lock);
                  return -1;
        }

        AMS_MUTEX_UNLOCK(&chip->lock);
      return 0;
}



static ssize_t app0_clk_trim_set_store(struct device * dev, struct device_attribute * attr, const char * buf, size_t count)
{

  struct tof_sensor_chip *chip = dev_get_drvdata(client_dev);
  int error;

  dev_info(dev, "%s: %s", __func__, buf);
  error = tof8801_rewrite_clock_trim_data(chip);
  
  return count;

}


static ssize_t app0_diag_state_mask_show(struct device *dev, struct device_attribute *attr,char *buf)
{
	int len;
	struct tof_sensor_chip *chip = dev_get_drvdata(client_dev);
	dev_info(dev, "%s\n", __func__);
	AMS_MUTEX_LOCK(&chip->lock);
	if (chip->info_rec.record.app_id != TOF8801_APP_ID_APP0) {
		dev_err(dev, "%s: Error ToF chip app_id: %#x",
						__func__, chip->info_rec.record.app_id);
		AMS_MUTEX_UNLOCK(&chip->lock);
		return -EINVAL;
	}
	len = scnprintf(buf, PAGE_SIZE, "%#x\n", chip->app0_app.diag_state_mask);
	AMS_MUTEX_UNLOCK(&chip->lock);
	return len;
}

static ssize_t app0_reflectivity_count_show(struct device *dev,struct device_attribute *attr,char *buf)
{
	int len;
	struct tof_sensor_chip *chip = dev_get_drvdata(client_dev);
	dev_info(dev, "%s\n", __func__);
	AMS_MUTEX_LOCK(&chip->lock);
	if (chip->info_rec.record.app_id != TOF8801_APP_ID_APP0) {
		dev_err(dev, "%s: Error ToF chip app_id: %#x",
						__func__, chip->info_rec.record.app_id);
		AMS_MUTEX_UNLOCK(&chip->lock);
		return -EINVAL;
	}
	len =
		scnprintf(buf, PAGE_SIZE, "object hits: %u\nreference hits: %u\n",
							chip->app0_app.algo_results_frame.results_frame.results_v2.data.objectHits,
							chip->app0_app.algo_results_frame.results_frame.results_v2.data.referenceHits);
	AMS_MUTEX_UNLOCK(&chip->lock);
	return len;
}

static ssize_t app0_general_configuration_show(struct device *dev,struct device_attribute *attr, char *buf)
{
	struct tof_sensor_chip *chip = dev_get_drvdata(client_dev);
	int error, i;
	int len = 0;
	dev_info(dev, "%s\n", __func__);
	AMS_MUTEX_LOCK(&chip->lock);
	if (chip->info_rec.record.app_id != TOF8801_APP_ID_APP0) {
		dev_err(dev, "%s: Error ToF chip app_id: %#x",
						__func__, chip->info_rec.record.app_id);
		AMS_MUTEX_UNLOCK(&chip->lock);
		return -EINVAL;
	}
	error = tof8801_app0_get_dataset(chip, GEN_CFG_DATASET);
	if (error) {
		AMS_MUTEX_UNLOCK(&chip->lock);
		return error;
	}
	for (i = 0; i < APP0_GENERAL_CONFIG_RSP_SIZE; i++) {
		len += scnprintf(buf + len, PAGE_SIZE - len, "%#x:%x\n", i,
										 chip->app0_app.dataset.buf[i]);
	}
	AMS_MUTEX_UNLOCK(&chip->lock);
	return len;
}

void dump_all_registers(struct device *dev)
{
	int i = 0;
	uint8_t buf[100] = {0};
	struct tof_sensor_chip *chip = dev_get_drvdata(client_dev);

	for (i = 0x00; i < 0x3b; i += 8) {
		tof_i2c_read(chip->client, i, &buf[i], 8);
		dev_info(dev, "0x%02x : 0x%02x, 0x%02x, 0x%02x , 0x%02x , 0x%02x , 0x%02x , 0x%02x , 0x%02x \n",i, buf[i], buf[i+1], buf[i+2], buf[i+3], buf[i+4], buf[i+5], buf[i+6], buf[i+7]);
	}
	tof_i2c_read(chip->client, i, &buf[i], 3);
	dev_info(dev, "0x%02x : 0x%02x, 0x%02x, 0x%02x \n",i, buf[i], buf[i+1], buf[i+2]);
	return;
}
#if 1
static ssize_t app0_read_peak_crosstalk_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	unsigned int len = 0;
	int i = 0;
	int error = 0;
	char peak_value[10] = {0};
	uint16_t peak_crosstalk[5] = {0};
	uint average = 0;
	struct tof_sensor_chip *chip = dev_get_drvdata(client_dev);

	dump_all_registers(dev);

	dev_info(dev, "%s\n", __func__);
	AMS_MUTEX_LOCK(&chip->lock);
	if (chip->info_rec.record.app_id != TOF8801_APP_ID_APP0) {
	  dev_err(dev, "%s: Error ToF chip app_id: %#x",
			  __func__, chip->info_rec.record.app_id);
	  AMS_MUTEX_UNLOCK(&chip->lock);
	  return -EIO;
	}

	for (i = 0; i < 10; i += 2) {
		msleep(70);
		error = tof_i2c_read(chip->client, 0x30, &peak_value[i], 2);

		if (error) {
		  dev_err(dev, "%s: Error i2c communication failure: %d", __func__, error);
		  AMS_MUTEX_UNLOCK(&chip->lock);
		  return error;
		}
		peak_crosstalk[i/2] = (uint16_t)(peak_value[i] << 8) | (uint16_t)peak_value[i+1];
		dev_info(dev, "peak_crosstalk[%d]=%04x\n", i/2, peak_crosstalk[i/2]);
		average += peak_crosstalk[i/2];
	}

	len = scnprintf(buf, PAGE_SIZE, "%d\n", average/5);
	
	AMS_MUTEX_UNLOCK(&chip->lock);
	return len;
}
#endif

#if 0
static ssize_t app0_read_peak_crosstalk_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	unsigned int len = 0;
	int i = 0;
	int error = 0;
	char peak_value[3] = {0};
	uint32_t peak_crosstalk;
	struct tof_sensor_chip *chip = dev_get_drvdata(client_dev);

	dump_all_registers(dev);

	dev_info(dev, "%s\n", __func__);
	AMS_MUTEX_LOCK(&chip->lock);
	if (chip->info_rec.record.app_id != TOF8801_APP_ID_APP0) {
	  dev_err(dev, "%s: Error ToF chip app_id: %#x",
			  __func__, chip->info_rec.record.app_id);
	  AMS_MUTEX_UNLOCK(&chip->lock);
	  return -EIO;
	}

	error = tof_i2c_read(chip->client, 0x20, &peak_value[i], 3);

	if (error) {
	  dev_err(dev, "%s: Error i2c communication failure: %d", __func__, error);
	  AMS_MUTEX_UNLOCK(&chip->lock);
	  return error;
	}
	peak_crosstalk = (uint32_t)(peak_value[i+2] << 12) | (uint32_t)(peak_value[i+1] << 4) | (uint32_t)(peak_value[i] >> 4);
	dev_info(dev, "peak_crosstalk=%d\n", peak_crosstalk);

	len = scnprintf(buf, PAGE_SIZE, "%d\n", peak_crosstalk);

	AMS_MUTEX_UNLOCK(&chip->lock);
	return len;
}
#endif
static ssize_t app0_apply_fac_calib_show(struct device *dev, struct device_attribute *attr,char *buf)
{
	struct tof_sensor_chip *chip = dev_get_drvdata(client_dev);
	int i;
	int len = 0;
	char *tmpbuf = (char *)&chip->ext_calib_data.fac_data;
	dev_info(dev, "%s\n", __func__);
	AMS_MUTEX_LOCK(&chip->lock);
	if (chip->info_rec.record.app_id != TOF8801_APP_ID_APP0) {
		dev_err(dev, "%s: Error ToF chip app_id: %#x",
						__func__, chip->info_rec.record.app_id);
		AMS_MUTEX_UNLOCK(&chip->lock);
		return -EINVAL;
	}
	for (i = 0; i < chip->ext_calib_data.size; i++) {
		len += scnprintf(buf + len, PAGE_SIZE - len, "fac_calib[%d]:%02x\n",
										 i, tmpbuf[i]);
	}
	AMS_MUTEX_UNLOCK(&chip->lock);
	return len;
}

static ssize_t app0_apply_fac_calib_store(struct device *dev, struct device_attribute *attr,const char *buf, size_t count)
{
	struct tof_sensor_chip *chip = dev_get_drvdata(client_dev);
//	int error = 0;
	dev_info(dev, "%s\n", __func__);
	AMS_MUTEX_LOCK(&chip->lock);
	if (chip->info_rec.record.app_id != TOF8801_APP_ID_APP0) {
		dev_err(dev, "%s: Error ToF chip app_id: %#x",
						__func__, chip->info_rec.record.app_id);
		AMS_MUTEX_UNLOCK(&chip->lock);
		return -EINVAL;
	}
	#if 0
	error = tof8801_get_fac_calib_data(chip);
	if (error) {
		AMS_MUTEX_UNLOCK(&chip->lock);
		return error;
	}
	#endif
	//set flag to update fac calib on next measure
	chip->app0_app.cal_update.dataFactoryConfig = 1;
	AMS_MUTEX_UNLOCK(&chip->lock);
	return count;
}

static ssize_t app0_apply_config_calib_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct tof_sensor_chip *chip = dev_get_drvdata(client_dev);
	int i;
	int len = 0;
	dev_info(dev, "%s\n", __func__);
	AMS_MUTEX_LOCK(&chip->lock);
	if (chip->info_rec.record.app_id != TOF8801_APP_ID_APP0) {
		dev_err(dev, "%s: Error ToF chip app_id: %#x",
						__func__, chip->info_rec.record.app_id);
		AMS_MUTEX_UNLOCK(&chip->lock);
		return -EINVAL;
	}
	for (i = 0; i < chip->config_data.size; i++) {
		len += scnprintf(buf + len, PAGE_SIZE - len, "config[%d]:%02x\n", i,
										 ((char *)&chip->config_data.cfg_data)[i]);
	}
	AMS_MUTEX_UNLOCK(&chip->lock);
	return len;
}

static ssize_t app0_apply_config_calib_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t count)
{
	struct tof_sensor_chip *chip = dev_get_drvdata(client_dev);
	int error = 0;
	dev_info(dev, "%s\n", __func__);
	AMS_MUTEX_LOCK(&chip->lock);
	if (chip->info_rec.record.app_id != TOF8801_APP_ID_APP0) {
		dev_err(dev, "%s: Error ToF chip app_id: %#x",
						__func__, chip->info_rec.record.app_id);
		AMS_MUTEX_UNLOCK(&chip->lock);
		return -EINVAL;
	}
	error = tof8801_get_config_calib_data(chip);
	if (error) {
		AMS_MUTEX_UNLOCK(&chip->lock);
		return error;
	}
	//set flag to update config calib data on next measure
	chip->app0_app.cal_update.dataConfiguration = 1;
	AMS_MUTEX_UNLOCK(&chip->lock);
	return count;
}

static ssize_t app0_apply_state_data_show(struct device *dev,struct device_attribute *attr, char *buf)
{
	struct tof_sensor_chip *chip = dev_get_drvdata(client_dev);
	int i;
	int len = 0;
	dev_info(dev, "%s\n", __func__);
	AMS_MUTEX_LOCK(&chip->lock);
	if (chip->info_rec.record.app_id != TOF8801_APP_ID_APP0) {
		dev_err(dev, "%s: Error ToF chip app_id: %#x",
						__func__, chip->info_rec.record.app_id);
		AMS_MUTEX_UNLOCK(&chip->lock);
		return -EINVAL;
	}
	for (i = 0; i < chip->alg_info.size; i++) {
		len += scnprintf(buf + len, PAGE_SIZE - len, "state_data[%d]:%02x\n", i,
										 ((char *)&chip->alg_info.alg_data.data)[i]);
	}
	AMS_MUTEX_UNLOCK(&chip->lock);
	return len;
}

static ssize_t app0_apply_state_data_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t count)
{
	struct tof_sensor_chip *chip = dev_get_drvdata(client_dev);
	int error = 0;
	int num = 0;
	char state[11] = {0};
	dev_info(dev, "%s\n", __func__);
	AMS_MUTEX_LOCK(&chip->lock);
	if (chip->info_rec.record.app_id != TOF8801_APP_ID_APP0) {
		dev_err(dev, "%s: Error ToF chip app_id: %#x",
						__func__, chip->info_rec.record.app_id);
		AMS_MUTEX_UNLOCK(&chip->lock);
		return -EINVAL;
	}
	memset(chip->alg_info.alg_data.data, 0, sizeof(chip->alg_info.alg_data));
	num = sscanf(buf, "%hhx %hhx %hhx %hhx %hhx %hhx %hhx %hhx %hhx %hhx %hhx",
							 &state[0], &state[1], &state[2], &state[3],
							 &state[4], &state[5], &state[6], &state[7],
							 &state[8], &state[9], &state[10]);
	memcpy(chip->alg_info.alg_data.data, state, sizeof(chip->alg_info.alg_data));
	chip->alg_info.size = 11;
	if (num != 11) {
		AMS_MUTEX_UNLOCK(&chip->lock);
		return error;
	}
	//set flag to update config calib data on next measure
	chip->app0_app.cal_update.dataAlgorithmState = 1;
	AMS_MUTEX_UNLOCK(&chip->lock);
	return count;
}

static ssize_t program_version_show(struct device *dev,struct device_attribute *attr, char *buf)
{
	struct tof_sensor_chip *chip = dev_get_drvdata(client_dev);
	int len = 0;
	dev_info(dev, "%s\n", __func__);
	AMS_MUTEX_LOCK(&chip->lock);
	if (chip->info_rec.record.app_id == TOF8801_APP_ID_APP0) {
		len = tof8801_app0_get_version(chip, buf, PAGE_SIZE);
		if (len == 0) {
			AMS_MUTEX_UNLOCK(&chip->lock);
			return -EIO;
		}
	} else {
		len = scnprintf(buf, PAGE_SIZE, "%#hhx-0-0-0\n",
										chip->info_rec.record.app_ver);
	}
	AMS_MUTEX_UNLOCK(&chip->lock);
	return len;
}

static ssize_t registers_show(struct device *dev,struct device_attribute *attr,char *buf)
{
	int per_line = 4;
	int len = 0;
	int idx, per_line_idx;
	int bufsize = PAGE_SIZE;
	int error;
	struct tof_sensor_chip *chip = dev_get_drvdata(client_dev);

	dev_info(dev, "%s\n", __func__);
	AMS_MUTEX_LOCK(&chip->lock);
	error = tof8801_get_all_regs(chip);
	if (error) {
		AMS_MUTEX_UNLOCK(&chip->lock);
		return error;
	}

	for (idx = 0; idx < MAX_REGS; idx += per_line) {
		len += scnprintf(buf + len, bufsize - len, "%#02x:", idx);
		for (per_line_idx = 0; per_line_idx < per_line; per_line_idx++) {
			len += scnprintf(buf + len, bufsize - len, " ");
			len += scnprintf(buf + len, bufsize - len, "%02x", chip->shadow[idx+per_line_idx]);
		}
		len += scnprintf(buf + len, bufsize - len, "\n");
	}
	AMS_MUTEX_UNLOCK(&chip->lock);
	return len;
}

static ssize_t app0_ctrl_reg_show(struct device *dev,struct device_attribute *attr,char *buf)
{
	struct tof_sensor_chip *chip = dev_get_drvdata(client_dev);
	int error, i;
	int len = 0;
	dev_dbg(dev, "%s\n", __func__);
	AMS_MUTEX_LOCK(&chip->lock);
	if (chip->info_rec.record.app_id != TOF8801_APP_ID_APP0) {
		dev_err(dev, "%s: Error ToF chip app_id: %#x",
						__func__, chip->info_rec.record.app_id);
		AMS_MUTEX_UNLOCK(&chip->lock);
		return -EINVAL;
	}
	//Read out APP0 header info: status, last cmd, TID, register contents, etc
	error = tof_i2c_read(chip->client, TOF8801_APP_ID,
											 chip->app0_app.ctrl_frame.buf,
											 sizeof(chip->app0_app.ctrl_frame.buf));
	if (error) {
		dev_err(dev, "%s: Error i2c communication failure: %d", __func__, error);
		AMS_MUTEX_UNLOCK(&chip->lock);
		return error;
	}
	for (i = 0; i < sizeof(chip->app0_app.ctrl_frame.buf); i++) {
		len += scnprintf(buf + len, PAGE_SIZE - len, "%#02x:%02x\n", i,
										 chip->app0_app.ctrl_frame.buf[i]);
	}
	AMS_MUTEX_UNLOCK(&chip->lock);
	return len;
}

static ssize_t register_write_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t count)
{
	char preg;
	char pval;
	char pmask = -1;
	int numparams;
	int rc;
	dev_info(dev, "%s\n", __func__);

	numparams = sscanf(buf, "%hhx:%hhx:%hhx", &preg, &pval, &pmask);
	if ((numparams < 2) || (numparams > 3))
		return -EINVAL;
	if ((numparams >= 1) && (preg < 0))
		return -EINVAL;
	if ((numparams >= 2) && (preg < 0 || preg > 0xff))
		return -EINVAL;
	if ((numparams >= 3) && (pmask < 0 || pmask > 0xff))
		return -EINVAL;

	if (pmask == -1) {
		rc = tof_i2c_write(to_i2c_client(dev), preg, &pval, 1);
	} else {
		rc = tof_i2c_write_mask(to_i2c_client(dev), preg, &pval, pmask);
	}

	return rc ? rc : count;
}

static ssize_t request_ram_patch_store(struct device *dev, struct device_attribute *attr,const char *buf, size_t count)
{
	struct tof_sensor_chip *chip = dev_get_drvdata(client_dev);
	int error = 0;
	dev_info(dev, "%s\n", __func__);
	AMS_MUTEX_LOCK(&chip->lock);
	/***** Make firmware download available to user space *****/
	error = tof8801_firmware_download(chip, 0);
	if (error) {
		AMS_MUTEX_UNLOCK(&chip->lock);
		return error;
	}
	/* enable all ToF interrupts on sensor */
	tof8801_enable_interrupts(chip, IRQ_RESULTS | IRQ_DIAG | IRQ_ERROR);

	AMS_MUTEX_UNLOCK(&chip->lock);
	return count;
}

static void tof8801_fw_work_rountine(struct work_struct *work)
{
	struct tof_sensor_chip *chip = container_of(work, struct tof_sensor_chip, fw_work);
	int error = 0;

	pr_info("%s enter.\n", __func__);
	AMS_MUTEX_LOCK(&chip->lock);
	error = tof8801_firmware_download(chip, 0);
	if (error) {
		AMS_MUTEX_UNLOCK(&chip->lock);
		pr_err("%s firmware download failed.\n", __func__);
		return ;
	}
	/*
	error = tof8801_get_config_calib_data(tof_chip);
	if (error) {
		pr_err(&client->dev, "Error reading config data: %d\n", error);
	}
	*/
	//read external (manufacturer) factory calibration data
	//error = tof8801_get_fac_calib_data(chip);
	//if (error) {
	//	pr_err("%s error reading fac_calib data: %d\n", __func__, error);
	//}

	AMS_MUTEX_UNLOCK(&chip->lock);

	//error = tof8801_rewrite_clock_trim_data(chip);
	//if (error) {
	//	pr_err("%s error tof8801_rewrite_clock_trim_data: %d\n", __func__, error);
	//}

}

static enum hrtimer_restart tof8801_fw_timer_func(struct hrtimer *timer)
{
	struct tof_sensor_chip *tof_chip = container_of(timer, struct tof_sensor_chip, fw_timer);

	pr_info("%s enter\n", __func__);

	schedule_work(&tof_chip->fw_work);

	return HRTIMER_NORESTART;
}

static int tof8801_fw_init(struct tof_sensor_chip *tof_chip)
{
	int fw_timer_val = 8000;

	hrtimer_init(&tof_chip->fw_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	tof_chip->fw_timer.function = tof8801_fw_timer_func;
	INIT_WORK(&tof_chip->fw_work, tof8801_fw_work_rountine);
	hrtimer_start(&tof_chip->fw_timer,ktime_set(fw_timer_val/1000, (fw_timer_val%1000)*1000000),HRTIMER_MODE_REL);
	return 0;
}
static ssize_t app0_get_fac_calib_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct tof_sensor_chip *chip = dev_get_drvdata(client_dev);
	int error;
	u32 len;
	unsigned long start = jiffies;
	int timeout_flag = 0;
	AMS_MUTEX_LOCK(&chip->lock);
	if (chip->info_rec.record.app_id != TOF8801_APP_ID_APP0) {
		dev_err(dev, "%s: Error ToF chip app_id: %#x", __func__, chip->info_rec.record.app_id);
		AMS_MUTEX_UNLOCK(&chip->lock);
		return -EINVAL;
	}
	//Stop any in-progress measurements
	(void) tof8801_app0_capture(chip, 0);
	#if 0
	error = tof8801_set_register(chip->client, OL_CMD_DATA6_OFFSET, 0xA7);
	if (error) {
	  dev_err(dev, "%s:set CMD_DATA6 register value error!!!", __func__);
	  AMS_MUTEX_UNLOCK(&chip->lock);
	  return 0;
	}
	#endif
	
	//set VCSEL clock to 20Mhz before calibration----alg set
	
	chip->app0_app.cap_settings.v2.alg = 0xA7;
	chip->app0_app.cap_settings.period = 66; //set period to 66ms
	*((__le16 *)chip->app0_app.cap_settings.iterations) = cpu_to_le16(900);//set iterations to 900k
	//start and then stop the measurement to let alg set take effect
	error = tof8801_app0_capture((void*)chip, 1);
	if (error) {
		dev_err(dev, "%s:start capture error!!!", __func__);
		AMS_MUTEX_UNLOCK(&chip->lock);
		return 0;
	}
	AMS_MUTEX_UNLOCK(&chip->lock);
	
	msleep(100);
	
	AMS_MUTEX_LOCK(&chip->lock);
	error = tof8801_app0_capture((void*)chip, 0);
	if (error) {
		dev_err(dev, "%s:stop capture error!!!", __func__);
		AMS_MUTEX_UNLOCK(&chip->lock);
		return 0;
	} 

	error = tof8801_app0_perform_factory_calibration(chip);
	if (error) {
		dev_err(dev, "Error starting factory calibration routine: %d", error);
		AMS_MUTEX_UNLOCK(&chip->lock);
		return 0;
	}
	do {
		//spin here waiting for factory calibration to complete
		AMS_MUTEX_UNLOCK(&chip->lock);
		msleep(100);
		AMS_MUTEX_LOCK(&chip->lock);
		timeout_flag = ((jiffies - start) >= msecs_to_jiffies(APP0_FAC_CALIB_MSEC_TIMEOUT));
	} while (!timeout_flag && tof8801_app0_measure_in_progress(chip));
	AMS_MUTEX_UNLOCK(&chip->lock);
	msleep(500);
	AMS_MUTEX_LOCK(&chip->lock);
	if (!tof8801_app0_measure_in_progress(chip) &&
			chip->app0_app.cal_update.dataFactoryConfig) {
		// If calib measure complete and was successful
		if (chip->ext_calib_data.size) {
			memcpy(buf, (void *)&chip->ext_calib_data.fac_data, chip->ext_calib_data.size);
		}
		len = chip->ext_calib_data.size;
		buf[len] = 0; //output is a string so we need to add null-terminating character
		dev_info(dev, "Done performing factory calibration, size: %u", len);
	} else {
		dev_err(dev, "Error timeout waiting on factory calibration");
		AMS_MUTEX_UNLOCK(&chip->lock);
		return 0;
	}
	AMS_MUTEX_UNLOCK(&chip->lock);
	orig_crosstalk = ((u32)buf[2] << 12) + ((u32)buf[1] << 4) + ((u32)buf[0] >> 4);
//	pr_info("tof8801 orig_crosstalk=%d",orig_crosstalk);
	if (orig_crosstalk > 50000) {
		return 0;
	}

	return len;
}

static ssize_t app0_tof_output_read(struct file *fp, struct kobject *kobj, struct bin_attribute *attr, char *buf, loff_t off, size_t size)
{
	struct device *dev = kobj_to_dev(kobj);
	struct tof_sensor_chip *chip = dev_get_drvdata(client_dev);
	int read;
	u32 elem_len;
	AMS_MUTEX_LOCK(&chip->lock);
	elem_len = kfifo_peek_len(&chip->tof_output_fifo);
	dev_dbg(dev, "%s size: %u\n", __func__, (unsigned int) size);
	if (kfifo_len(&chip->tof_output_fifo)) {
		dev_dbg(dev, "fifo read elem_len: %u\n", elem_len);
		read = kfifo_out(&chip->tof_output_fifo, buf, elem_len);
		dev_dbg(dev, "fifo_len: %u\n", kfifo_len(&chip->tof_output_fifo));
		AMS_MUTEX_UNLOCK(&chip->lock);
		return elem_len;
	} else {
		AMS_MUTEX_UNLOCK(&chip->lock);
		return 0;
	}
}

static ssize_t clk_trim_firmware_download_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	unsigned int len = 0;
	
	dev_info(dev, "%s, clock trim calibration value = 0x%02x\n", __func__, clk_trim_firmware);
	len += scnprintf(buf + len, PAGE_SIZE - len, "clock_trim_calibration:0x%02x\n", clk_trim_firmware);
	
	return len;
}

static ssize_t clk_trim_firmware_download_store(struct device * dev, struct device_attribute * attr, const char * buf, size_t count)
{

	struct tof_sensor_chip *chip = dev_get_drvdata(client_dev);
	int error;

	dev_info(dev, "%s:0x%02x\n", __func__, *buf);
	memcpy((void *)(&clk_trim_firmware), buf, sizeof(clk_trim_firmware));
	dev_info(dev, "%s: 0x%02x\n", __func__, clk_trim_firmware);
	error = tof8801_rewrite_clock_trim_data(chip);
	if (error) {
		pr_err("%s error tof8801_rewrite_clock_trim_data: %d\n", __func__, error);
		return error;
	}

	return count;

}

static ssize_t crosstalk_firmware_download_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct tof_sensor_chip *chip = dev_get_drvdata(client_dev);
	int i;
	int len = 0;
	char *tmpbuf = (char *)&chip->ext_calib_data.fac_data;
	dev_info(dev, "%s\n", __func__);
	AMS_MUTEX_LOCK(&chip->lock);
	if (chip->info_rec.record.app_id != TOF8801_APP_ID_APP0) {
		dev_err(dev, "%s: Error ToF chip app_id: %#x",
						__func__, chip->info_rec.record.app_id);
		AMS_MUTEX_UNLOCK(&chip->lock);
		return -EINVAL;
	}
	for (i = 0; i < chip->ext_calib_data.size; i++) {
		len += scnprintf(buf + len, PAGE_SIZE - len, "fac_calib[%d]:0x%02x\n", i, tmpbuf[i]);
	}
	AMS_MUTEX_UNLOCK(&chip->lock);
	return len;
}

static ssize_t crosstalk_firmware_download_store(struct device * dev, struct device_attribute * attr, const char * buf, size_t count)
{
	int i;
	char *tmpbuf ;
	struct tof_sensor_chip *chip = dev_get_drvdata(client_dev);
	
	for(i = 0; i <count; i++)
		  dev_info(&chip->client->dev,"=== hope buf [%d] = 0x%02x\n", i, buf[i]);
	memcpy(crosstalk_firmware, buf, TOF_GET_FAC_CALIB_BIN_SIZE);
	memcpy((void *)&chip->ext_calib_data.fac_data,buf, TOF_GET_FAC_CALIB_BIN_SIZE);
	chip->ext_calib_data.size = TOF_GET_FAC_CALIB_BIN_SIZE;
	orig_crosstalk = ((u32)crosstalk_firmware[2] << 12) + ((u32)crosstalk_firmware[1] << 4) + ((u32)crosstalk_firmware[0] >> 4);
	tmpbuf = (char *)&chip->ext_calib_data.fac_data;
	  for (i = 0; i < chip->ext_calib_data.size; i++) {
		  dev_info(&chip->client->dev,"=== hope fac_calib[%d] = 0x%02x\n", i, tmpbuf[i]);
	  }

  return count;

}

/****************************************************************************
 * Common Sysfs Attributes
 * **************************************************************************/
/******* READ-WRITE attributes ******/
static DEVICE_ATTR_RW(program);
static DEVICE_ATTR_RW(chip_enable);
static DEVICE_ATTR_RW(driver_debug);
/******* READ-ONLY attributes ******/
static DEVICE_ATTR_RO(program_version);
static DEVICE_ATTR_RO(registers);
/******* WRITE-ONLY attributes ******/
static DEVICE_ATTR_WO(register_write);
static DEVICE_ATTR_WO(request_ram_patch);

/****************************************************************************
 * Bootloader Sysfs Attributes
 * **************************************************************************/
/******* READ-WRITE attributes ******/
/******* READ-ONLY attributes ******/
/******* WRITE-ONLY attributes ******/

/****************************************************************************
 * APP0 Sysfs Attributes
 * *************************************************************************/
/******* READ-WRITE attributes ******/
static DEVICE_ATTR_RW(app0_command);
static DEVICE_ATTR_RW(capture);
static DEVICE_ATTR_RW(period);
static DEVICE_ATTR_RW(noise_threshold);
static DEVICE_ATTR_RW(iterations);
static DEVICE_ATTR_RW(capture_delay);
static DEVICE_ATTR_RW(alg_setting);
static DEVICE_ATTR_RW(gpio_setting);
static DEVICE_ATTR_RW(app0_clk_iterations);
static DEVICE_ATTR_RW(app0_clk_trim_enable);
static DEVICE_ATTR_RW(app0_clk_trim_set);
static DEVICE_ATTR_RW(app0_apply_fac_calib);
static DEVICE_ATTR_RW(app0_apply_config_calib);
static DEVICE_ATTR_RW(app0_apply_state_data);
static DEVICE_ATTR_RW(clk_trim_firmware_download);
static DEVICE_ATTR_RW(crosstalk_firmware_download);
/******* READ-ONLY attributes ******/
static DEVICE_ATTR_RO(app0_general_configuration);
static DEVICE_ATTR_RO(app0_ctrl_reg);
static DEVICE_ATTR_RO(app0_temp);
static DEVICE_ATTR_RO(app0_diag_state_mask);
static DEVICE_ATTR_RO(app0_reflectivity_count);
static DEVICE_ATTR_RO(app0_get_fac_calib);
static DEVICE_ATTR_RO(app0_read_peak_crosstalk);
/******* WRITE-ONLY attributes ******/
/******* READ-ONLY BINARY attributes ******/
static BIN_ATTR_RO(app0_tof_output, 0);

static struct attribute *tof_common_attrs[] = {
	&dev_attr_program.attr,
	&dev_attr_chip_enable.attr,
	&dev_attr_driver_debug.attr,
	&dev_attr_program_version.attr,
	&dev_attr_registers.attr,
	&dev_attr_register_write.attr,
	&dev_attr_request_ram_patch.attr,
	NULL,
};
static struct attribute *tof_bl_attrs[] = {
	NULL,
};
static struct attribute *tof_app0_attrs[] = {
	&dev_attr_app0_command.attr,
	&dev_attr_capture.attr,
	&dev_attr_period.attr,
	&dev_attr_iterations.attr,
	&dev_attr_noise_threshold.attr,
	&dev_attr_capture_delay.attr,
	&dev_attr_alg_setting.attr,
	&dev_attr_gpio_setting.attr,
	&dev_attr_app0_clk_iterations.attr,
	&dev_attr_app0_clk_trim_enable.attr,
	&dev_attr_app0_clk_trim_set.attr,
	&dev_attr_app0_diag_state_mask.attr,
	&dev_attr_app0_general_configuration.attr,
	&dev_attr_app0_ctrl_reg.attr,
	&dev_attr_app0_temp.attr,
	&dev_attr_app0_reflectivity_count.attr,
	&dev_attr_app0_get_fac_calib.attr,
	&dev_attr_app0_apply_fac_calib.attr,
	&dev_attr_app0_apply_config_calib.attr,
	&dev_attr_app0_apply_state_data.attr,
	&dev_attr_app0_read_peak_crosstalk.attr,
	&dev_attr_clk_trim_firmware_download.attr,
	&dev_attr_crosstalk_firmware_download.attr,
	NULL,
};
static struct bin_attribute *tof_app0_bin_attrs[] = {
	&bin_attr_app0_tof_output,
	NULL,
};
static const struct attribute_group tof_common_group = {
	.attrs = tof_common_attrs,
};
static const struct attribute_group tof_bl_group = {
	.name = "bootloader",
	.attrs = tof_bl_attrs,
};
static const struct attribute_group tof_app0_group = {
	.name = "app0",
	.attrs = tof_app0_attrs,
	.bin_attrs = tof_app0_bin_attrs,
};
static const struct attribute_group *tof_groups[] = {
	&tof_common_group,
	&tof_bl_group,
	&tof_app0_group,
	NULL,
};

/**
 * tof_i2c_read - Read number of bytes starting at a specific address over I2C
 *
 * @client: the i2c client
 * @reg: the i2c register address
 * @buf: pointer to a buffer that will contain the received data
 * @len: number of bytes to read
 */
int tof_i2c_read(struct i2c_client *client, char reg, char *buf, int len)
{
	struct i2c_msg msgs[2];
	int ret;

	msgs[0].flags = 0;
	msgs[0].addr  = client->addr;
	msgs[0].len   = 1;
	msgs[0].buf   = &reg;

	msgs[1].flags = I2C_M_RD;
	msgs[1].addr  = client->addr;
	msgs[1].len   = len;
	msgs[1].buf   = buf;

	ret = i2c_transfer(client->adapter, msgs, 2);
	return ret < 0 ? ret : (ret != ARRAY_SIZE(msgs) ? -EIO : 0);
}

/**
 * tof_i2c_write - Write nuber of bytes starting at a specific address over I2C
 *
 * @client: the i2c client
 * @reg: the i2c register address
 * @buf: pointer to a buffer that will contain the data to write
 * @len: number of bytes to write
 */
int tof_i2c_write(struct i2c_client *client, char reg, const char *buf, int len)
{
	u8 *addr_buf;
	struct i2c_msg msg;
	struct tof_sensor_chip *chip = i2c_get_clientdata(client);
	int idx = reg;
	int ret;
	char debug[120];
	u32 strsize = 0;

	addr_buf = kmalloc(len + 1, GFP_KERNEL);
	if (!addr_buf)
		return -ENOMEM;

	addr_buf[0] = reg;
	memcpy(&addr_buf[1], buf, len);
	msg.flags = 0;
	msg.addr = client->addr;
	msg.buf = addr_buf;
	msg.len = len + 1;

	ret = i2c_transfer(client->adapter, &msg, 1);
	if (ret != 1) {
		dev_err(&client->dev, "i2c_transfer failed: %d msg_len: %u", ret, len);
	}
	if (chip->driver_debug > 1) {
		strsize = scnprintf(debug, sizeof(debug), "i2c_write: ");
		for (idx = 0; (ret == 1) && (idx < msg.len); idx++) {
			strsize += scnprintf(debug + strsize, sizeof(debug) - strsize, "%02x ", addr_buf[idx]);
		}
		dev_info(&client->dev, "%s", debug);
	}

	kfree(addr_buf);
	return ret < 0 ? ret : (ret != 1 ? -EIO : 0);
}

/**
 * tof_i2c_write_mask - Write a byte to the specified address with a given bitmask
 *
 * @client: the i2c client
 * @reg: the i2c register address
 * @val: byte to write
 * @mask: bitmask to apply to address before writing
 */
int tof_i2c_write_mask(struct i2c_client *client, char reg,
											 const char *val, char mask)
{
	int ret;
	u8 temp;
	struct tof_sensor_chip *chip = i2c_get_clientdata(client);

	ret = tof_i2c_read(client, reg, &temp, 1);
	temp &= ~mask;
	temp |= *val;
	ret = tof_i2c_write(client, reg, &temp, 1);

	chip->shadow[(int)reg] = temp;

	return ret;
}

/**
 * tof8801_get_register - Return a specific register
 *
 * @chip: tof_sensor_chip pointer
 * @value: pointer to value in register
 */
int tof8801_get_register(struct i2c_client *client,
																char reg, char *value)
{
	return tof_i2c_read(client, reg, value, sizeof(char));
}

/**
 * tof8801_set_register - Set a specific register
 *
 * @chip: tof_sensor_chip pointer
 * @value: value to set in register
 */
int tof8801_set_register(struct i2c_client *client,
																char reg, const char value)
{
	return tof_i2c_write(client, reg, &value, sizeof(char));
}

/**
 * tof8801_set_register_mask - Set a specific register, with a mask
 *
 * @chip: tof_sensor_chip pointer
 * @value: value to set in register
 * @mask: mask to apply with register, i.e. value=0x1, mask=0x1 = only bit 0 set
 */
int tof8801_set_register_mask(struct i2c_client *client,
																		 char reg, const char value, const char mask)
{
	return tof_i2c_write_mask(client, reg, &value, mask);
}

void tof_dump_i2c_regs(struct tof_sensor_chip *chip, char offset, char end)
{
	int per_line = 4;
	int len = 0;
	int idx, per_line_idx;
	char debug[80];

	offset &= ~(per_line - 1); // Byte boundary for nice printing
	while ((end & (per_line - 1)) != (per_line - 1))
		end += 1;
	end = (end < offset) ? (offset+per_line) : end;
	dev_info(&chip->client->dev, "%s\n", __func__);
	(void) tof_i2c_read(chip->client, offset,
											&chip->shadow[(int)offset],
											(end - offset));
	for (idx = offset; idx < end; idx += per_line) {
		memset(debug, 0, sizeof(debug));
		len += scnprintf(debug, sizeof(debug) - len, "%02x: ", idx);
		for (per_line_idx = 0; per_line_idx < per_line; per_line_idx++) {
			len += scnprintf(debug + len, sizeof(debug) - len, "%02x ", chip->shadow[idx+per_line_idx]);
		}
		len = 0;
		dev_info(&chip->client->dev, "%s", debug);
	}
}

/**
 * tof_standby_operation - Tell the ToF chip to wakeup/standby
 *
 * @client: the i2c client
 */
static int tof_standby_operation(struct i2c_client *client, char oper)
{
	return tof8801_set_register(client, TOF8801_STAT, oper);
}

/**
 * tof_CE_toggle - Hard reset the ToF by toggling the ChipEnable
 *
 * @client: the i2c client
 */
static int tof_CE_toggle(struct i2c_client *client)
{
	struct tof_sensor_chip *chip = i2c_get_clientdata(client);
	int error = 0;
	if (!chip->pdata->gpiod_enable) {
		//not supported in poll mode
		return -EIO;
	}
	error = gpiod_direction_output(chip->pdata->gpiod_enable, 0);
	if (error)
		return error;
	error = gpiod_direction_output(chip->pdata->gpiod_enable, 1);
	/* ToF requires 5ms to get i2c back up */
	usleep_range(TOF8801_I2C_WAIT_USEC, TOF8801_I2C_WAIT_USEC+1);
	return error;
}

/**
 * tof_wait_for_cpu_ready_timeout - Check for CPU ready state in the ToF sensor
 *                                  for a specified period of time
 *
 * @client: the i2c client
 */
int tof_wait_for_cpu_ready_timeout(struct i2c_client *client, unsigned long usec)
{
	int error = 0;
	unsigned long curr = jiffies;
	do {
		error = tof_wait_for_cpu_ready(client);
		if (error == 0) {
			return 0;
		}
	} while ((jiffies - curr) < usecs_to_jiffies(usec));
	dev_err(&client->dev, "Error timeout (%lu usec) waiting on cpu_ready: %d\n", usec, error);
	return -EIO;
}

/**
 * tof_wait_for_cpu_ready - Check for CPU ready state in the ToF sensor
 *
 * @client: the i2c client
 */
int tof_wait_for_cpu_ready(struct i2c_client *client)
{
	int retry = 0;
	int error;
	u8 status;

	//wait for i2c
	usleep_range(TOF8801_I2C_WAIT_USEC, TOF8801_I2C_WAIT_USEC+1);
	while (retry++ < TOF8801_MAX_WAIT_RETRY) {
		error = tof8801_get_register(client, TOF8801_STAT, &status);
		if (error) {
			dev_err(&client->dev, "i2c test failed attempt %d: %d\n", retry, error);
			continue;
		}
		if (TOF8801_STAT_CPU_READY(status)) {
			dev_dbg(&client->dev, "ToF chip CPU is ready");
			return 0;
		} else if (TOF8801_STAT_CPU_SLEEP(status)) {
			dev_info(&client->dev, "ToF chip in standby state, waking up");
			tof_standby_operation(client, WAKEUP);
			usleep_range(TOF8801_I2C_WAIT_USEC, TOF8801_I2C_WAIT_USEC+1);
			error = -EIO;
			continue;
		} else if (TOF8801_STAT_CPU_BUSY(status) &&
							(retry >= TOF8801_MAX_WAIT_RETRY)) {
			return -EIO;
		}
		usleep_range(TOF8801_WAIT_UDELAY, 2*TOF8801_WAIT_UDELAY);
	}
	return error;
}

/**
 * tof_wait_for_cpu_startup - Check for CPU ready state in the ToF sensor
 *
 * @client: the i2c client
 */
int tof_wait_for_cpu_startup(struct i2c_client *client)
{
	int retry = 0;
	int CE_retry = 0;
	int error;
	u8 status;

	while (retry++ < TOF8801_MAX_STARTUP_RETRY) {
		usleep_range(TOF8801_I2C_WAIT_USEC, TOF8801_I2C_WAIT_USEC+1);
		error = tof8801_get_register(client, TOF8801_STAT, &status);
		if (error) {
			dev_err(&client->dev, "i2c test failed attempt %d: %d\n", retry, error);
			continue;
		} else {
			dev_dbg(&client->dev, "CPU status register: %#04x value: %#04x\n",
							 TOF8801_STAT, status);
		}
		if (TOF8801_STAT_CPU_READY(status)) {
			dev_info(&client->dev, "ToF chip CPU is ready");
			return 0;
		} else if (TOF8801_STAT_CPU_SLEEP(status)) {
			dev_info(&client->dev, "ToF chip in standby state, waking up");
			tof_standby_operation(client, WAKEUP);
			error = -EIO;
			continue;
		} else if (TOF8801_STAT_CPU_BUSY(status) &&
							(retry >= TOF8801_MAX_STARTUP_RETRY)) {
			if ((CE_retry < TOF8801_MAX_STARTUP_RETRY)) {
				dev_info(&client->dev, "ToF chip still busy, try toggle CE");
				if (tof_CE_toggle(client)) {
					return -EIO;
				}
				retry = 0;
				CE_retry++;
			} else {
				return -EIO;
			}
		}
	}
	return error;
}

/**
 * tof_init_info_record - initialize info record of currently running app
 *
 * @client: the i2c client
 * @record: pointer to info_record struct where data will be placed
 */
int tof_init_info_record(struct tof_sensor_chip *chip)
{
	int error;

	error = tof_i2c_read(chip->client, TOF8801_APP_ID, chip->info_rec.data, TOF8801_INFO_RECORD_SIZE);
	if (error) {
		dev_err(&chip->client->dev, "read record failed: %d\n", error);
		goto err;
	}
	dev_info(&chip->client->dev, "Read info record - Running app_id: %#x.\n", chip->info_rec.record.app_id);
	/* re-initialize apps */
	if (chip->info_rec.record.app_id == TOF8801_APP_ID_BOOTLOADER) {
		tof8801_BL_init_app(&chip->BL_app);
	} else if (chip->info_rec.record.app_id == TOF8801_APP_ID_APP0) {
		tof8801_app0_init_app(&chip->app0_app);
	}
	return 0;
err:
	return error;
}

static int tof_switch_from_bootloader(struct tof_sensor_chip *chip, char req_app_id)
{
	int error = 0;
	char *new_app_id;

	// Try to perform RAM download (if possible)
	error = tof8801_firmware_download(chip, 0);
	if (error != 0) {
		//This means either there is no firmware, or there was a failure
		error = tof8801_set_register(chip->client, TOF8801_REQ_APP_ID, req_app_id);
		if (error) {
			dev_err(&chip->client->dev, "Error setting REQ_APP_ID register.\n");
			error = -EIO;
		}
		error = tof_wait_for_cpu_ready_timeout(chip->client, 100000);
		if (error) {
			dev_err(&chip->client->dev, "Error waiting for CPU ready flag.\n");
		}
		error = tof_init_info_record(chip);
		if (error) {
			dev_err(&chip->client->dev, "Error reading info record.\n");
		}
	}
	new_app_id = &chip->info_rec.record.app_id;
	dev_info(&chip->client->dev, "Running app_id: 0x%02x\n", *new_app_id);
	switch (*new_app_id) {
	case TOF8801_APP_ID_BOOTLOADER:
		dev_err(&chip->client->dev, "Error: application switch failed.\n");
		break;
	case TOF8801_APP_ID_APP0:
		/* enable all ToF interrupts on sensor */
		tof8801_enable_interrupts(chip, IRQ_RESULTS | IRQ_DIAG | IRQ_ERROR);
		break;
	case TOF8801_APP_ID_APP1:
		break;
	default:
		dev_err(&chip->client->dev, "Error: Unrecognized application.\n");
		return -EINVAL;
	}
	return (*new_app_id == req_app_id) ? 0 : -1;
}

int tof_switch_apps(struct tof_sensor_chip *chip, char req_app_id)
{
	int error = 0;
	if (req_app_id == chip->info_rec.record.app_id)
		return 0;
	if ((req_app_id != TOF8801_APP_ID_BOOTLOADER) &&
			(req_app_id != TOF8801_APP_ID_APP0)       &&
			(req_app_id != TOF8801_APP_ID_APP1))
		return -EINVAL;
	switch (chip->info_rec.record.app_id) {
	case TOF8801_APP_ID_BOOTLOADER:
		error = tof_switch_from_bootloader(chip, req_app_id);
		if (error) {
			/* Hard reset back to bootloader if error */
			gpiod_set_value(chip->pdata->gpiod_enable, 0);
			gpiod_set_value(chip->pdata->gpiod_enable, 1);
			error = tof_wait_for_cpu_startup(chip->client);
			if (error) {
				dev_err(&chip->client->dev,
								"I2C communication failure: %d\n",
								error);
				return error;
			}
			error = tof_init_info_record(chip);
			if (error) {
				dev_err(&chip->client->dev,
								"Read application info record failed.\n");
				return error;
			}
			return -EINVAL;
		}
		break;
	case TOF8801_APP_ID_APP0:
		error = tof8801_app0_switch_to_bootloader(chip);
		break;
	case TOF8801_APP_ID_APP1:
		return -EINVAL;
		break;
	}
	return error;
}

/**
 * tof_hard_reset - use GPIO Chip Enable to reset the device
 *
 * @tof_chip: tof_sensor_chip pointer
 */
int tof_hard_reset(struct tof_sensor_chip *chip)
{
	int error = 0;
	int in_app0 = (chip->info_rec.record.app_id == TOF8801_APP_ID_APP0);
	if (!chip->pdata->gpiod_enable) {
		return -EIO;
	}

	//Try to stop measurements cleanly
	if (in_app0 && (gpiod_get_value(chip->pdata->gpiod_enable) == 1)) {
		(void)tof8801_app0_capture(chip, 0);
	}
	/* toggle CE pin */
	gpiod_set_value(chip->pdata->gpiod_enable, 0);
	gpiod_set_value(chip->pdata->gpiod_enable, 1);

	error = tof_wait_for_cpu_startup(chip->client);
	if (error) {
		dev_err(&chip->client->dev, "I2C communication failure: %d\n", error);
		return error;
	}
	error = tof_init_info_record(chip);
	if (error) {
		dev_err(&chip->client->dev, "Read application info record failed.\n");
		return error;
	}
	// If we were in App0 before CE LOW, try to download/switch back to App0
	if (in_app0) {
		// Need to perform RAM download if CE is toggled
		error = tof_switch_apps(chip, (char)TOF8801_APP_ID_APP0);
		if (error) {
			return error;
		}
	}
	return error;
}

/**
 * tof_get_gpio_config - Get GPIO config from DT
 *
 * @tof_chip: tof_sensor_chip pointer
 */
static int tof_get_gpio_config(struct tof_sensor_chip *tof_chip)
{
	int error;
	struct device *dev;
	struct gpio_desc *gpiod;

	if (!tof_chip->client)
		return -EINVAL;
	dev = &tof_chip->client->dev;

	/* Get the enable line GPIO pin number */
	gpiod = devm_gpiod_get_optional(dev, TOF_GPIO_ENABLE_NAME, GPIOD_OUT_HIGH);
	if (IS_ERR(gpiod)) {
		error = PTR_ERR(gpiod);
		return error;
	}
	tof_chip->pdata->gpiod_enable = gpiod;

	/* Get the interrupt GPIO pin number */
	gpiod = devm_gpiod_get_optional(dev, TOF_GPIO_INT_NAME, GPIOD_IN);
	if (IS_ERR(gpiod)) {
		error = PTR_ERR(gpiod);
		return error;
	}
	tof_chip->pdata->gpiod_interrupt = gpiod;

	/*
	gpiod = of_get_named_gpio(dev->of_node, "irq-gpio", 0);
	if (rc < 0) {
	  dev_err(&client->dev, "Couldn't get Interrupt GPIO.\n");
	  return error;
	}
	tof_chip->pdata->gpiod_interrupt = gpiod;
	*/
	return 0;
}

/**
 * tof_ram_patch_callback - The firmware download callback
 *
 * @cfg: the firmware cfg structure
 * @ctx: private data pointer to struct tof_sensor_chip
 */
static void tof_ram_patch_callback(const struct firmware *cfg, void *ctx)
{
	struct tof_sensor_chip *chip = ctx;
	const u8 *line;
	const u8 *line_end;
	int verify = 0;
	int result = 0;
	u32 patch_size = 0;
	u64 fwdl_time = 0;
	struct timespec start_ts = {0}, end_ts = {0};
	AMS_MUTEX_LOCK(&chip->lock);
	if (!chip) {
		pr_err("AMS-TOF Error: Ram patch callback NULL context pointer.\n");
	}

	if (!cfg) {
		dev_warn(&chip->client->dev, "%s: Warning, firmware not available.\n", __func__);
		goto err_fwdl;
	}
	dev_info(&chip->client->dev, "%s: Ram patch in progress...\n", __func__);
	/* Assuming you can only perform ram download while in BL application */
	/* switch back to BL app to perform RAM download */
	if (chip->info_rec.record.app_id != TOF8801_APP_ID_BOOTLOADER) {
		dev_info(&chip->client->dev,
						 "Current app_id: %hhx - Switching to bootloader for RAM download",
						 chip->info_rec.record.app_id);
		result = tof_switch_apps(chip, (char)TOF8801_APP_ID_BOOTLOADER);
		if (result) {
			dev_info(&chip->client->dev, "Error changing to bootloader app: \'%d\'", result);
			goto err_fwdl;
		}
	}
	//Start fwdl timer
	getnstimeofday(&start_ts);
	/* setup encryption salt */
	result = tof8801_BL_upload_init(chip->client, &chip->BL_app, tof_salt_value);
	if (result) {
		dev_info(&chip->client->dev, "Error setting upload salt: \'%d\'", result);
		goto err_fwdl;
	}
	//assume we have mutex already
	intelHexInterpreterInitialise();
	line = cfg->data;
	line_end = line;
	while ((line_end - cfg->data) < cfg->size) {
		line_end = strchrnul(line, '\n');
		patch_size += ((line_end - line) > INTEL_HEX_MIN_RECORD_SIZE) ?((line_end - line - INTEL_HEX_MIN_RECORD_SIZE) / 2) : 0;
		result = intelHexHandleRecord(chip->client, &chip->BL_app, line_end - line, line, verify);
		if (result) {
			dev_err(&chip->client->dev, "%s: Ram patch failed: %d\n", __func__, result);
			goto err_fwdl;
		}
		line = ++line_end;
	}
	//Stop fwdl timer
	getnstimeofday(&end_ts);
	fwdl_time = timespec_sub(end_ts, start_ts).tv_nsec / 1000000; //time in ms
	dev_info(&chip->client->dev, "%s: Ram patch complete, patch size: %uK, dl time: %llu ms\n", __func__, ((patch_size >> 10) + 1), fwdl_time);
	//wait for i2c
	usleep_range(TOF8801_I2C_WAIT_USEC, TOF8801_I2C_WAIT_USEC+1);
	/* resync our info record since we just switched apps */
	tof_init_info_record(chip);
err_fwdl:
	release_firmware(cfg);
	complete_all(&chip->ram_patch_in_progress);
	AMS_MUTEX_UNLOCK(&chip->lock);
}

int tof_queue_frame(struct tof_sensor_chip *chip, void *buf, int size)
{
	unsigned int fifo_len;
	unsigned int frame_size;
	int result = kfifo_in(&chip->tof_output_fifo, buf, size);
	if (result == 0) {
		if (chip->driver_debug == 1)
			dev_err(&chip->client->dev, "Error: Frame buffer is full, clearing buffer.\n");
		kfifo_reset(&chip->tof_output_fifo);
		tof8801_app0_report_error(chip, ERR_BUF_OVERFLOW, DEV_OK);
		result = kfifo_in(&chip->tof_output_fifo, buf, size);
		if (result == 0) {
			dev_err(&chip->client->dev, "Error: queueing ToF output frame.\n");
		}
	}
	if (chip->driver_debug == 2) {
		fifo_len = kfifo_len(&chip->tof_output_fifo);
		frame_size = ((char *)buf)[DRV_FRAME_SIZE_LSB] |
								 (((char *)buf)[DRV_FRAME_SIZE_MSB] << 8);
		dev_info(&chip->client->dev, "Add frame_id: 0x%x, data_size: %u\n",
						 ((char *)buf)[DRV_FRAME_ID], frame_size);
		dev_info(&chip->client->dev,
						 "New fifo len: %u, fifo utilization: %u%%\n",
						 fifo_len, (1000*fifo_len/kfifo_size(&chip->tof_output_fifo))/10);
	}
	return (result == size) ? 0 : -1;
}

/**
 * tof_irq_handler - The IRQ handler
 *
 * @irq: interrupt number.
 * @dev_id: private data pointer.
 */
static irqreturn_t tof_irq_handler(int irq, void *dev_id)
{
	struct tof_sensor_chip *tof_chip = (struct tof_sensor_chip *)dev_id;
	char int_stat = 0;
	char appid;
	int error;

//	dev_info(&tof_chip->client->dev, "Enter IRQ Handler.\n");
	AMS_MUTEX_LOCK(&tof_chip->lock);
	//Go to appropriate IRQ handler depending on the app running
	appid = tof_chip->info_rec.record.app_id;
	switch (appid) {
	case TOF8801_APP_ID_BOOTLOADER:
		goto irq_handled;
	case TOF8801_APP_ID_APP0:
		(void)tof8801_get_register(tof_chip->client, TOF8801_INT_STAT, &int_stat);
		if (tof_chip->driver_debug) {
			dev_info(&tof_chip->client->dev, "IRQ stat: %#x\n", int_stat);
		}
		if (int_stat != 0) {
			//Clear interrupt on ToF chip
			error = tof8801_set_register(tof_chip->client, TOF8801_INT_STAT, int_stat);
			if (error) {
				tof8801_app0_report_error(tof_chip, ERR_COMM, DEV_OK);
			}
			tof8801_app0_process_irq(tof_chip, int_stat);
			/* Alert user space of changes */
			sysfs_notify(&tof_chip->client->dev.kobj,
									 tof_app0_group.name,
									 bin_attr_app0_tof_output.attr.name);
		}
		break;
	case TOF8801_APP_ID_APP1:
		goto irq_handled;
	}
irq_handled:
	AMS_MUTEX_UNLOCK(&tof_chip->lock);
	return IRQ_HANDLED;
}

int tof8801_app0_poll_irq_thread(void *tof_chip)
{
	struct tof_sensor_chip *chip = (struct tof_sensor_chip *)tof_chip;
	char meas_cmd = 0;
	int us_sleep = 0;
	struct sched_param param = { .sched_priority = MAX_RT_PRIO - 1 };
	sched_setscheduler(chip->app0_poll_irq, SCHED_FIFO, &param);
	AMS_MUTEX_LOCK(&chip->lock);
	// Poll period is interpreted in units of 100 usec
	us_sleep = chip->poll_period * 100;
	dev_info(&chip->client->dev, "Starting ToF irq polling thread, period: %u us\n", us_sleep);
	AMS_MUTEX_UNLOCK(&chip->lock);
	while (!kthread_should_stop()) {
		AMS_MUTEX_LOCK(&chip->lock);
		meas_cmd = chip->app0_app.cap_settings.cmd;
		AMS_MUTEX_UNLOCK(&chip->lock);
		if (meas_cmd) {
			(void) tof_irq_handler(0, tof_chip);
		}
		usleep_range(us_sleep, us_sleep + us_sleep/10);
	}
	return 0;
}

/**
 * tof_request_irq - request IRQ for given gpio
 *
 * @tof_chip: tof_sensor_chip pointer
 */
static int tof_request_irq(struct tof_sensor_chip *tof_chip)
{
	int irq = tof_chip->client->irq;
	unsigned long default_trigger = irqd_get_trigger_type(irq_get_irq_data(irq));
	dev_info(&tof_chip->client->dev, "irq: %d, trigger_type: %lu", irq, default_trigger);
	return devm_request_threaded_irq(&tof_chip->client->dev, tof_chip->client->irq, NULL, tof_irq_handler, IRQF_TRIGGER_FALLING | IRQF_SHARED | IRQF_ONESHOT, tof_chip->client->name, tof_chip);
}

/**
 * tof8801_get_all_regs - read all addressable I2C registers from device
 *
 * @tof_chip: tof_sensor_chip pointer
 */
static int tof8801_get_all_regs(struct tof_sensor_chip *tof_chip)
{
	int error;

	memset(tof_chip->shadow, 0, MAX_REGS);
	error = tof_i2c_read(tof_chip->client, TOF8801_APP_ID, tof_chip->shadow, MAX_REGS);
	if (error < 0) {
		dev_err(&tof_chip->client->dev, "Read all registers failed: %d\n", error);
		return error;
	}
	return 0;
}

/**
 * tof8801_enable_interrupts - enable specified interrutps
 *
 * @tof_chip: tof_sensor_chip pointer
 * @int_en_flags: OR'd flags of interrupts to enable
 */
static int tof8801_enable_interrupts(struct tof_sensor_chip *chip, char int_en_flags)
{
	char flags;
	int error = tof8801_get_register(chip->client, TOF8801_INT_EN, &flags);
	flags &= TOF8801_INT_MASK;
	flags |= int_en_flags;
	if (error) {
		return error;
	}
	return tof8801_set_register(chip->client, TOF8801_INT_EN, flags);
}

static int tof8801_get_config_calib_data(struct tof_sensor_chip *chip)
{
	int error;
	const struct firmware *config_fw = NULL;
	/* Set current configuration calibration data size to 0*/
	chip->config_data.size = 0;
	///***** Check for available fac_calib to read *****/
	error = request_firmware_direct(&config_fw,
																	chip->pdata->config_calib_data_fname,
																	&chip->client->dev);
	if (error || !config_fw) {
		dev_warn(&chip->client->dev,
						 "configuration calibration data not available \'%s\': %d\n",
						 chip->pdata->config_calib_data_fname, error);
		return 0;
	} else {
		dev_info(&chip->client->dev, "Read in config_calib file: \'%s\'.\n",
						 chip->pdata->config_calib_data_fname);
	}
	if (config_fw->size > sizeof(chip->config_data.cfg_data)) {
		dev_err(&chip->client->dev,
						"Error: config calibration data size too large %zu > %zu (MAX)\n",
						config_fw->size, sizeof(chip->config_data.cfg_data));
		return 1;
	}
	memcpy((void *)&chip->config_data.cfg_data,
				 config_fw->data, config_fw->size);
	chip->config_data.size = config_fw->size;
	release_firmware(config_fw);
	return 0;
}

#if 0
static int tof8801_get_fac_calib_data(struct tof_sensor_chip *chip)
{
	int error;
	const struct firmware *calib_fw = NULL;
	/* Set current factory calibration data size to 0*/
	chip->ext_calib_data.size = 0;
	//Alg info is only valid with factory cal, so clear it as well
	chip->alg_info.size = 0;
	///***** Check for available fac_calib to read *****/

	//error = request_firmware_direct(&calib_fw,
	error = request_firmware(&calib_fw,
							chip->pdata->fac_calib_data_fname,
							&chip->client->dev);

	/*
	error = request_firmware_nowait(THIS_MODULE, true,
									chip->pdata->fac_calib_data_fname,
									&chip->client->dev, GFP_KERNEL, chip,
									NULL);
	*/
	if (error) {
		dev_warn(&chip->client->dev,
						 "factory calibration data not available \'%s\': %d\n",
						 chip->pdata->fac_calib_data_fname, error);
		return 0;
	} else {
		dev_info(&chip->client->dev, "Read in fac_calib file: \'%s\'.\n",
						 chip->pdata->fac_calib_data_fname);
	}
	
	if (calib_fw->size > sizeof(chip->ext_calib_data.fac_data)) {
		dev_err(&chip->client->dev,
						"Error: factory calibration data size too large %zu > %zu (MAX)\n",
						calib_fw->size, sizeof(chip->ext_calib_data.fac_data));
		return 1;
	}
	
	memcpy((void *)&chip->ext_calib_data.fac_data,
				 calib_fw->data, calib_fw->size);
	chip->ext_calib_data.size = calib_fw->size;
	release_firmware(calib_fw);
	dev_info(&chip->client->dev, "fac_calib file loaded successfully.\n");
	return 0;
}

#endif

#if 0
static int tof8801_get_fac_calib_data(struct tof_sensor_chip *chip)
{
	int error;
	struct timespec start_ts = {0}, end_ts = {0};
	int mutex_locked = mutex_is_locked(&chip->lock);
	getnstimeofday(&start_ts);
	/* Iterate through all Firmware(s) to find one that works. 'Works' here is
	 * defined as running APP0 after FWDL
	 */
	reinit_completion(&chip->ram_patch_in_progress);
	if (mutex_locked) {
		AMS_MUTEX_UNLOCK(&chip->lock);
	}
	
	dev_info(&chip->client->dev, "Trying firmware: \'%s\'...\n",
					 chip->pdata->fac_calib_data_fname);
	/***** Check for available firmware to load *****/
	error = request_firmware_nowait(THIS_MODULE, true,
																	chip->pdata->fac_calib_data_fname,
																	&chip->client->dev, GFP_KERNEL, chip,
																	NULL);
	if (error) {
		dev_warn(&chip->client->dev, "Firmware not available \'%s\': %d\n",
						 chip->pdata->fac_calib_data_fname, error);
	}
	if (!wait_for_completion_interruptible_timeout(&chip->ram_patch_in_progress,
												msecs_to_jiffies(TOF_FWDL_TIMEOUT_MSEC))) {
		dev_err(&chip->client->dev, "Timeout waiting for Ram Patch \'%s\' Complete",
						chip->pdata->fac_calib_data_fname);
	}
	if (mutex_locked) {
		AMS_MUTEX_LOCK(&chip->lock);
	}
	getnstimeofday(&end_ts);
	dev_dbg(&chip->client->dev, "FWDL callback %lu ms to finish",
					(timespec_sub(end_ts, start_ts).tv_nsec / 1000000));
	// error if App0 is not running (fwdl failed)
	return (chip->info_rec.record.app_id != TOF8801_APP_ID_APP0) ? -EIO : 0;
}
#endif

static int tof8801_firmware_download(struct tof_sensor_chip *chip, int startup)
{
	int error;
	struct timespec start_ts = {0}, end_ts = {0};
	int mutex_locked = mutex_is_locked(&chip->lock);
	int file_idx = 0;
	
	getnstimeofday(&start_ts);
	/* Iterate through all Firmware(s) to find one that works. 'Works' here is
	 * defined as running APP0 after FWDL
	 */
	 
	for (file_idx = 0; chip->pdata->ram_patch_fname[file_idx] != NULL; file_idx++) {
		/*** reset completion event that FWDL is starting ***/
		reinit_completion(&chip->ram_patch_in_progress);
		if (mutex_locked) {
			AMS_MUTEX_UNLOCK(&chip->lock);
		}
		
		dev_info(&chip->client->dev, "Trying firmware: \'%s\'...\n", chip->pdata->ram_patch_fname[file_idx]);
		/***** Check for available firmware to load *****/
		error = request_firmware_nowait(THIS_MODULE, true,chip->pdata->ram_patch_fname[file_idx],&chip->client->dev, GFP_KERNEL, chip,tof_ram_patch_callback);
		if (error) {
			laser_running = 0;
			dev_warn(&chip->client->dev, "Firmware not available \'%s\': %d\n",chip->pdata->ram_patch_fname[file_idx], error);
		}
		if (!startup &&!wait_for_completion_interruptible_timeout(&chip->ram_patch_in_progress,msecs_to_jiffies(TOF_FWDL_TIMEOUT_MSEC))) {
			dev_err(&chip->client->dev, "Timeout waiting for Ram Patch \'%s\' Complete",chip->pdata->ram_patch_fname[file_idx]);
		}
		if (mutex_locked) {
			AMS_MUTEX_LOCK(&chip->lock);
		}
		if (chip->info_rec.record.app_id == TOF8801_APP_ID_APP0) {
			// assume we are done if APP0 is running
			break;
		}

		
	}
	

	getnstimeofday(&end_ts);
	dev_info(&chip->client->dev, "FWDL callback %lu ms to finish", (timespec_sub(end_ts, start_ts).tv_nsec / 1000000));
	// error if App0 is not running (fwdl failed)
	return (chip->info_rec.record.app_id != TOF8801_APP_ID_APP0) ? -EIO : 0;
}

static int tof_input_dev_open(struct input_dev *dev)
{
	struct tof_sensor_chip *chip = input_get_drvdata(dev);
	int error = 0;
	dev_info(&dev->dev, "%s\n", __func__);
	AMS_MUTEX_LOCK(&chip->lock);
	if (chip->pdata->gpiod_enable && (gpiod_get_value(chip->pdata->gpiod_enable) == 0)) {
		/* enable the chip */
		error = gpiod_direction_output(chip->pdata->gpiod_enable, 1);
		if (error) {
			dev_err(&dev->dev, "Chip enable failed.\n");
			AMS_MUTEX_UNLOCK(&chip->lock);
			return -EIO;
		}
	}
	AMS_MUTEX_UNLOCK(&chip->lock);
	return error;
}

static int tof_probe(struct i2c_client *client, const struct i2c_device_id *idp)
{
	struct tof_sensor_chip *tof_chip;
	int error = 0;
	void *poll_prop_ptr = NULL;

	dev_info(&client->dev, "I2C Address: %#04x\n", client->addr);
	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C)) {
		dev_err(&client->dev, "I2C check functionality failed.\n");
		return -ENXIO;
	}

	tof_chip = devm_kzalloc(&client->dev, sizeof(*tof_chip), GFP_KERNEL);
	if (!tof_chip)
		return -ENOMEM;
	
	tmf8801_chip = tof_chip;

	/***** Setup data structures *****/
	mutex_init(&tof_chip->lock);
	client->dev.platform_data = (void *)&tof_pdata;
	tof_chip->client = client;
	tof_chip->pdata = &tof_pdata;
	i2c_set_clientdata(client, tof_chip);
	client_dev = &client->dev;
	/***** Firmware sync structure initialization*****/
	init_completion(&tof_chip->ram_patch_in_progress);
	//initialize kfifo for frame output
	INIT_KFIFO(tof_chip->tof_output_fifo);
	//Setup measure timer
	setup_timer(&tof_chip->meas_timer,tof8801_app0_measure_timer_expiry_callback,(unsigned long) tof_chip);
	//Setup input device
	tof_chip->obj_input_dev = devm_input_allocate_device(&client->dev);
	if (tof_chip->obj_input_dev == NULL) {
		dev_err(&client->dev, "Error allocating input_dev.\n");
		goto input_dev_alloc_err;
	}
	tof_chip->obj_input_dev->name = tof_chip->pdata->tof_name;
	tof_chip->obj_input_dev->id.bustype = BUS_I2C;
	input_set_drvdata(tof_chip->obj_input_dev, tof_chip);
	tof_chip->obj_input_dev->open = tof_input_dev_open;
	set_bit(EV_ABS, tof_chip->obj_input_dev->evbit);
	input_set_abs_params(tof_chip->obj_input_dev, ABS_DISTANCE, 0, 0xFF, 0, 0);
//	tof_chip->driver_debug = 1;
	error = input_register_device(tof_chip->obj_input_dev);
	if (error) {
		dev_err(&client->dev, "Error registering input_dev.\n");
		goto input_reg_err;
	}

	error = tof_get_gpio_config(tof_chip);
	if (error)
		goto gpio_err;

	/***** Set ChipEnable HIGH *****/
	if (tof_chip->pdata->gpiod_enable) {
		/* enable the chip */
		error = gpiod_direction_output(tof_chip->pdata->gpiod_enable, 1);
		if (error) {
			dev_err(&client->dev, "Chip enable failed.\n");
			goto gpio_err;
		}
	}
	poll_prop_ptr = (void *)of_get_property(tof_chip->client->dev.of_node,TOF_PROP_NAME_POLLIO,NULL);
	tof_chip->poll_period = poll_prop_ptr ? be32_to_cpup(poll_prop_ptr) : 0;
	if (tof_chip->poll_period == 0) {
		/*** Use Interrupt I/O instead of polled ***/
		/***** Setup GPIO IRQ handler *****/
		dev_info(&client->dev, "Use Interrupt I/O instead of polled\n");
		if (tof_chip->pdata->gpiod_interrupt) {
			error = tof_request_irq(tof_chip);
			if (error) {
				dev_err(&client->dev, "Interrupt request Failed.\n");
				goto gen_err;
			}
		}

	} else {
		/*** Use Polled I/O instead of interrupt ***/
		dev_info(&client->dev, "Use Polled I/O instead of interrupt\n");
		tof_chip->app0_poll_irq = kthread_run(tof8801_app0_poll_irq_thread, (void *)tof_chip, "tof-irq_poll");
		if (IS_ERR(tof_chip->app0_poll_irq)) {
			dev_err(&client->dev, "Error starting IRQ polling thread.\n");
			error = PTR_ERR(tof_chip->app0_poll_irq);
			goto kthread_start_err;
		}
	}

	/***** Wait until ToF is ready for commands *****/
	error = tof_wait_for_cpu_startup(client);
	if (error) {
		dev_err(&client->dev, "I2C communication failure: %d\n", error);
		goto gen_err;
	}

	tof_chip->saved_clk_trim = UNINITIALIZED_CLK_TRIM_VAL;
	//read external (manufacturer) configuration data
	

	tof8801_app0_default_cap_settings(&tof_chip->app0_app);
	
	//add init value for tof8801_app0_freq_ratio_loth_Q15 and tof8801_app0_freq_ratio_hith_Q15
	 tof_chip->tof8801_app0_freq_ratio_loth_Q15 = 6903;
	 tof_chip->tof8801_app0_freq_ratio_hith_Q15 = 7041;

	error = tof_init_info_record(tof_chip);
	if (error) {
		dev_err(&client->dev, "Read application info record failed.\n");
		goto gen_err;
	}
#if 0
	error = sysfs_create_groups(&client->dev.kobj, tof_groups);
	if (error) {
		dev_err(&client->dev, "Error creating sysfs attribute group.\n");
		goto sysfs_err;
	}
#endif
	/* enable all ToF interrupts on sensor */
	tof8801_enable_interrupts(tof_chip, IRQ_RESULTS | IRQ_DIAG | IRQ_ERROR);
	tof8801_fw_init(tof_chip);
	dev_info(&client->dev, "Probe ok.laser_running =%d\n", laser_running);
	return 0;

	/***** Failure case(s), unwind and return error *****/
#if 0
sysfs_err:
	sysfs_remove_groups(&client->dev.kobj, tof_groups);
#endif
gen_err:
	if (tof_chip->poll_period != 0) {
		(void)kthread_stop(tof_chip->app0_poll_irq);
	}
gpio_err:
	if (tof_chip->pdata->gpiod_enable)
		(void) gpiod_direction_output(tof_chip->pdata->gpiod_enable, 0);
kthread_start_err:
input_dev_alloc_err:
input_reg_err:
	i2c_set_clientdata(client, NULL);
	laser_running = 0;
	dev_info(&client->dev, "Probe failed.\n");
	return error;
}

static int tof_suspend(struct device *dev)
{
	/*
	struct tof_sensor_chip *chip = dev_get_drvdata(client_dev);
	dev_info(dev, "%s\n", __func__);
	AMS_MUTEX_LOCK(&chip->lock);
	if (!chip->pdata->gpiod_enable) {
		AMS_MUTEX_UNLOCK(&chip->lock);
		return -EIO;
	}
	gpiod_set_value(chip->pdata->gpiod_enable, 0);
	AMS_MUTEX_UNLOCK(&chip->lock);
	*/
	return 0;
}

static int tof_resume(struct device *dev)
{
	/*
	struct tof_sensor_chip *chip = dev_get_drvdata(client_dev);
	int error = 0;
	dev_info(dev, "%s\n", __func__);
	AMS_MUTEX_LOCK(&chip->lock);
	if (!chip->pdata->gpiod_enable) {
		AMS_MUTEX_UNLOCK(&chip->lock);
		return -EIO;
	}
	error = tof_hard_reset(chip);
	if (error) {
		dev_err(&chip->client->dev, "Error issuing Reset-HARD");
		AMS_MUTEX_UNLOCK(&chip->lock);
		return -EIO;
	}
	AMS_MUTEX_UNLOCK(&chip->lock);
	*/
	return 0;
}

static void tof8801_recovery_work_rountine(struct work_struct *work)
{
	int error = 0;

	pr_info("%s enter.\n", __func__);

	tof8801_enable_interrupts(tmf8801_chip, IRQ_RESULTS | IRQ_DIAG | IRQ_ERROR);
	AMS_MUTEX_LOCK(&tmf8801_chip->lock);
	error = tof8801_firmware_download(tmf8801_chip, 0);
	AMS_MUTEX_UNLOCK(&tmf8801_chip->lock);
	if (error) {
		pr_err("%s firmware download failed.\n", __func__);
		return;
	}

	error = tof8801_rewrite_clock_trim_data(tmf8801_chip);
	if (error) {
		pr_err("%s error tof8801_rewrite_clock_trim_data: %d\n", __func__, error);
		return;
	}

	tmf8801_chip->app0_app.cal_update.dataFactoryConfig = 1;

	driver_debug = 0;

	if (laser_running == 1) {
		AMS_MUTEX_LOCK(&tmf8801_chip->lock);
		tmf8801_chip->app0_app.cap_settings.v2.alg = 0xA7;
		tmf8801_chip->app0_app.cap_settings.period = 66;//set period to 66ms
		*((__le16 *)tmf8801_chip->app0_app.cap_settings.iterations) = cpu_to_le16(900);//set iterations to 900k
		error = tof8801_app0_capture((void *)tmf8801_chip, 1);
		if (error) {
			pr_info("%s:start capture error!!!", __func__);
			AMS_MUTEX_UNLOCK(&tmf8801_chip->lock);
			return;
		}
		AMS_MUTEX_UNLOCK(&tmf8801_chip->lock);
	} else{
		pr_info("laser is bad,  laser_running =%d\n", laser_running);
	}

}

static int tof8801_recovery(struct tof_sensor_chip *tof_chip)
{
	int fw_timer_val = 500;

	pr_info("%s enter.\n", __func__);
	hrtimer_init(&tof_chip->fw_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	tof_chip->fw_timer.function = tof8801_fw_timer_func;
	INIT_WORK(&tof_chip->fw_work, tof8801_recovery_work_rountine);
	hrtimer_start(&tof_chip->fw_timer, ktime_set(fw_timer_val/1000, (fw_timer_val%1000)*1000000), HRTIMER_MODE_REL);
	return 0;
}

static void tof8801_clean_distance(void)
{
	tof_core2_result_t *result = &tmf8801_chip->app0_app.algo_results_frame.results_frame.results_v2;

	result->data.distPeak = 0;
	result->data.resultInfo.reliabilityE = 0;

}

int tof8801_resume(void)
{
	int error = 0;
	u8 tof_mode;

	if (is_resume == 1) {
		pr_info("tof8801  has resumed \n");
		return 0;
	}
	is_resume = 1;

	if (laser_running == 1) {
		tof8801_clean_distance();
		error = tof_i2c_read(tmf8801_chip->client, 0x02, &tof_mode, 1);
		if (error < 0) {
			dev_err(&tmf8801_chip->client->dev, "Read all registers failed: %d\n", error);
			return error;
		}
		if (tof_mode != TOF8801_APP_ID_APP0) {
			tof8801_recovery(tmf8801_chip);
			return -1;
		}
		if (orig_crosstalk == 0) {
			pr_info("tof8801_resume  orig_crosstalk =%d\n", orig_crosstalk);
			laser_running = 0;
		}
	}

	driver_debug = 0;

	pr_info("tof8801_resume  start, laser_running = %d\n",  laser_running);

	if (laser_running == 1) {
		AMS_MUTEX_LOCK(&tmf8801_chip->lock);
		tmf8801_chip->app0_app.cap_settings.v2.alg = 0xA7;
		tmf8801_chip->app0_app.cap_settings.period = 66;//set period to 66ms
		*((__le16 *)tmf8801_chip->app0_app.cap_settings.iterations) = cpu_to_le16(900);//set iterations to 900k
		error = tof8801_app0_capture((void *)tmf8801_chip, 1);
		if (error) {
			pr_info("%s:start capture error!!!", __func__);
			AMS_MUTEX_UNLOCK(&tmf8801_chip->lock);
			return -1;
		}
		AMS_MUTEX_UNLOCK(&tmf8801_chip->lock);
	}else{
		pr_info("laser is bad,  laser_running =%d\n", laser_running);
		return -1;
	}
	pr_info("tof8801_resume  end  laser_running = %d\n", laser_running);
	return 0;
}

int tof8801_suspend(void)
{
	int error = 0;

	if (is_resume == 0) {
		pr_info("tof8801  has suspended \n");
		return 0;
	}
	is_resume = 0;

	if (laser_running == 1) {
		if (orig_crosstalk == 0) {
			pr_info("tof8801_suspend  orig_crosstalk =%d\n",  orig_crosstalk);
			laser_running = 0;
		}
	}

	pr_info("tof8801_suspend  start, laser_running =%d\n", laser_running);

	if (laser_running == 1) {
		tof8801_clean_distance();
		AMS_MUTEX_LOCK(&tmf8801_chip->lock);
		error = tof8801_app0_capture((void *)tmf8801_chip, 0);
		if (error) {
			pr_info("%s:stop capture error!!!", __func__);
			AMS_MUTEX_UNLOCK(&tmf8801_chip->lock);
			return -1;
		}
		AMS_MUTEX_UNLOCK(&tmf8801_chip->lock);
	}else{
		pr_info("laser is bad,  laser_running =%d\n", laser_running);
		return -1;
	}
	pr_info("tof8801_suspend  end , laser_running =%d \n", laser_running);
	return 0;
}

int tof8801_get_distance(u32 *distance_mm, u32 *confidence)
{

	if (laser_running == 1) {
		tof_core2_result_t * result = &tmf8801_chip->app0_app.algo_results_frame.results_frame.results_v2;

		*distance_mm = (u32)(result->data.distPeak & 0xFFFF);
		*confidence = (u32)((result->data.resultInfo.reliabilityE) & 0x3f);
	}else{
		*distance_mm = 0;
		*confidence = 0;
		if (!driver_debug) {
			pr_info("===hope laser is bad,  tof8801_get_distance(), laser_running =%d, *distance_mm =%d, *confidence =%d\n", laser_running, *distance_mm, *confidence); 
			driver_debug =1;
		}
		return -1;
	}
//	pr_info("===hope distance =  0x%04x, confidence =0x%02x\n",  *distance_mm, *confidence);
	return 0;
}


static int tof_remove(struct i2c_client *client)
{
	struct tof_sensor_chip *chip = i2c_get_clientdata(client);
	char int_stat = 0;
	if (chip->info_rec.record.app_id == TOF8801_APP_ID_APP0) {
		//Stop any current measurements
		tof8801_app0_capture(chip, 0);
		(void)tof8801_get_register(chip->client, TOF8801_INT_STAT, &int_stat);
		if (int_stat != 0) {
			//Clear any interrupt status
			(void) tof8801_set_register(chip->client,TOF8801_INT_STAT, int_stat);
		}
	}
	if (chip->pdata->gpiod_interrupt) {
		devm_free_irq(&client->dev, client->irq, chip);
		devm_gpiod_put(&client->dev, chip->pdata->gpiod_interrupt);
	}
	if (chip->poll_period != 0) {
		(void)kthread_stop(chip->app0_poll_irq);
	}
	if (chip->pdata->gpiod_enable) {
		/* disable the chip */
		(void) gpiod_direction_output(chip->pdata->gpiod_enable, 0);
		devm_gpiod_put(&client->dev, chip->pdata->gpiod_enable);
	}
	//sysfs_remove_groups(&client->dev.kobj, (const struct attribute_group **)&tof_groups);
	del_timer_sync(&chip->meas_timer); //delete measure timer
	dev_info(&client->dev, "%s\n", __func__);
	i2c_set_clientdata(client, NULL);
	return 0;
}

static struct i2c_device_id tof_idtable[] = {
	{ "tof8801", 0 },
	{}
};
MODULE_DEVICE_TABLE(i2c, tof_idtable);

static const struct dev_pm_ops tof_pm_ops = {
	.suspend = tof_suspend,
	.resume  = tof_resume,
};

static const struct of_device_id tof_of_match[] = {
	{ .compatible = "ams,tof8801" },
	{ }
};
MODULE_DEVICE_TABLE(of, tof_of_match);

static struct i2c_driver tof_driver = {
	.driver = {
		.name = "ams-tof",
		.pm = &tof_pm_ops,
		.of_match_table = of_match_ptr(tof_of_match),
	},
	.id_table = tof_idtable,
	.probe = tof_probe,
	.remove = tof_remove,
};

//module_i2c_driver(tof_driver);

static int tof8801_probe(struct platform_device *pdev)
{
	int error = 0;
	pr_info("===hope Probe start.\n");
	laser_running = 1;
	if (i2c_add_driver(&tof_driver)) {
		pr_info("==hope Failed to add i2c driver.\n");
		laser_running = 0;
		return -1;
	}
	
	error = sysfs_create_groups(&pdev->dev.kobj, tof_groups);	
	if (error != 0 || laser_running ==0) {
		dev_err(&pdev->dev, "==== hope Error creating sysfs attribute group.\n");
		goto sysfs_err;
	}
	pr_info("===hope Probe done.\n");
	return 0;
	
sysfs_err:
		sysfs_remove_groups(&pdev->dev.kobj, tof_groups);
		laser_running = 0;
		pr_info("===hope Probe  failed.\n");
	return 0;
}

static int tof8801_remove(struct platform_device *pdev)
{
	pr_info("Remove start.\n");
	i2c_del_driver(&tof_driver);
	pr_info("Remove done.\n");

	return 0;
}

static const struct of_device_id tof8801_of_match[] = {
	{.compatible = "samsung,tof8801-ams",},
	{},
};
MODULE_DEVICE_TABLE(of, tof8801_of_match);

static struct platform_driver tof8801_platform_driver = {
	.probe = tof8801_probe,
	.remove = tof8801_remove,
	.driver = {
		.name = TOF8801_AMS_NAME,
		.owner = THIS_MODULE,
		.of_match_table = tof8801_of_match,
	},
};

static int __init tof8801_ams_init(void)
{
	int ret;
	pr_info("Init start.\n");
	ret = platform_driver_register(&tof8801_platform_driver);
	if (ret) {
		pr_err("Failed to register platform driver\n");
		return ret;
	}
	pr_info("Init done.\n");
	return 0;
}

static void __exit tof8801_ams_exit(void)
{
	pr_info("Exit start.\n");
	platform_driver_unregister(&tof8801_platform_driver);
	pr_info("Exit done.\n");
}

module_init(tof8801_ams_init);
module_exit(tof8801_ams_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("AMS-TAOS tmf8801 ToF sensor driver");
MODULE_VERSION("3.11");
