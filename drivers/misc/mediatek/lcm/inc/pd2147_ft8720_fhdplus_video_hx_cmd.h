/*
 * Copyright (c) 2015 MediaTek Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#define FRAME_WIDTH				(1080)
#define FRAME_HEIGHT			(2408)
#define VIVO_DATA_RATE			(1106)
#define VIVO_PLL_CLOCK			(553)
#define VIVO_PIXEL_CLOCK		(171893)/*v_total * h_total *60 / 1000 */
#define MIPI_LANE				(4)
#define VIVO_REFRESH_RATE		(60)

#define LCM_PHYSICAL_WIDTH			(68526)
#define LCM_PHYSICAL_HEIGHT			(152450)
/* physical size in um */
#define LCM_DENSITY				(440)


#define REGFLAG_DELAY			0xFFFC
#define REGFLAG_UDELAY			0xFFFB
#define REGFLAG_END_OF_TABLE	0xFFFD
#define REGFLAG_RESET_LOW		0xFFFE
#define REGFLAG_RESET_HIGH		0xFFFF

#define VFP (20)
#define VSA (10)
#define VBP (32)
#define HFP (20)
#define HSA (10)
#define HBP (36)

#define VIVO_FREQB_DATA_RATE	(1097)
#define HFPFB 					(26)		/*hbp for mipi hop Freq B*/

static struct LCD_setting_table lcm_suspend_setting[] = {
	{0x28,1, {0x00} },
	{REGFLAG_DELAY, 20, {} },
	{0x10,1, {0x00} },
	{REGFLAG_DELAY, 100, {} },
};

static struct LCD_setting_table lcm_all_pixel_off_setting[] = {
	{0x22,1,{0x00} },
};

static struct LCD_setting_table lcm_all_pixel_on_setting[] = {
	{0x13,1,{0x00}},
};

static struct LCD_setting_table init_setting[] = {
{0x00,1,{0x00}},
{0xFF,3,{0x87,0x20,0x01}},
{0x00,1,{0x80}},
{0xFF,2,{0x87,0x20}},
{0x00,1,{0xA3}},
{0xB3,4,{0x09,0x68,0x00,0x18}},
{0x00,1,{0x80}},
{0xC0,6,{0x00,0x48,0x00,0x2D,0x00,0x11}},
{0x00,1,{0x90}},
{0xC0,6,{0x00,0x48,0x00,0x2D,0x00,0x11}},
{0x00,1,{0xA0}},
{0xC0,6,{0x00,0x4E,0x00,0x2D,0x00,0x11}},
{0x00,1,{0xB0}},
{0xC0,5,{0x00,0xBB,0x00,0x2D,0x11}},
{0x00,1,{0x60}},
{0xC0,6,{0x00,0x8E,0x00,0x2D,0x00,0x11}},
{0x00,1,{0x70}},
{0xC0,12,{0x00,0x78,0x00,0x80,0x0D,0x02,0xB6,0x00,0x00,0x15,0x00,0xE8}},
{0x00,1,{0xA3}},
{0xC1,6,{0x00,0x6E,0x00,0x3E,0x00,0x02}},
{0x00,1,{0x80}},
{0xCE,16,{0x01,0x81,0xFF,0xFF,0x01,0x08,0x01,0x08,0x00,0x00,0x00,0x00,0x01,0x78,0x01,0x88}},
{0x00,1,{0x90}},
{0xCE,15,{0x00,0xA7,0x10,0x43,0x00,0xA7,0x80,0xFF,0xFF,0x00,0x06,0x40,0x0C,0x0F,0x00}},
{0x00,1,{0xA0}},
{0xCE,3,{0x11,0x00,0x00}},
{0x00,1,{0xB0}},
{0xCE,3,{0x22,0x00,0x00}},
{0x00,1,{0xD1}},
{0xCE,7,{0x00,0x00,0x01,0x00,0x00,0x00,0x00}},
{0x00,1,{0xE1}},
{0xCE,11,{0x04,0x02,0xB6,0x02,0xB6,0x00,0x00,0x00,0x00,0x00,0x00}},
{0x00,1,{0xF1}},
{0xCE,9,{0x2A,0x2A,0x00,0x00,0xE8,0x00,0xE8,0x00,0x00}},
{0x00,1,{0xB0}},
{0xCF,4,{0x00,0x00,0x64,0x68}},
{0x00,1,{0xB5}},
{0xCF,4,{0x04,0x04,0xE4,0xE8}},
{0x00,1,{0xC0}},
{0xCF,4,{0x09,0x09,0x63,0x67}},
{0x00,1,{0xC5}},
{0xCF,4,{0x09,0x09,0x69,0x6D}},
{0x00,1,{0xD1}},
{0xC1,12,{0x0A,0xDF,0x0F,0x1C,0x19,0xCB,0x0A,0xDF,0x0F,0x2D,0x19,0xE6}},
{0x00,1,{0xE1}},
{0xC1,2,{0x0F,0x1C}},
{0x00,1,{0xE4}},
{0xCF,12,{0x09,0xF9,0x09,0xF8,0x09,0xF8,0x09,0xF8,0x09,0xF8,0x09,0xF8}},
{0x00,1,{0x80}},
{0xC1,2,{0x00,0x00}},
{0x00,1,{0x90}},
{0xC1,1,{0x03}},
{0x00,1,{0xF5}},
{0xCF,1,{0x02}},
{0x00,1,{0xF6}},
{0xCF,1,{0x3C}},
{0x00,1,{0xF0}},
{0xCF,2,{0x01,0x3C}},
{0x00,1,{0xF0}},
{0xC1,1,{0x00}},
{0x00,1,{0xCC}},
{0xC1,1,{0x18}},
{0x00,1,{0xE0}},
{0xC1,1,{0x00}},
{0x00,1,{0x00}},
{0x1C,1,{0x02}},
{0x00,1,{0x80}},
{0xC2,8,{0x84,0x00,0x05,0x8B,0x83,0x00,0x05,0x8B}},
{0x00,1,{0xA0}},
{0xC2,15,{0x82,0x04,0x00,0x05,0x8B,0x81,0x04,0x00,0x05,0x8B,0x80,0x04,0x00,0x05,0x8B}},
{0x00,1,{0xB0}},
{0xC2,15,{0x01,0x04,0x00,0x05,0x8B,0x02,0x04,0x00,0x05,0x8B,0x03,0x04,0x00,0x05,0x8B}},
{0x00,1,{0xC0}},
{0xC2,10,{0x04,0x04,0x00,0x05,0x8B,0x05,0x04,0x00,0x05,0x8B}},
{0x00,1,{0xE0}},
{0xC2,4,{0x77,0x77,0x77,0x77}},
{0x00,1,{0xE8}},
{0xC2,8,{0x18,0x99,0x6B,0x75,0x00,0x00,0x00,0x00}},
{0x00,1,{0xD0}},
{0xC3,16,{0x35,0x0A,0x00,0x00,0x35,0x0A,0x00,0x00,0x35,0x0A,0x00,0x00,0x00,0x00,0x00,0x00}},
{0x00,1,{0xE0}},
{0xC3,16,{0x35,0x0A,0x00,0x00,0x35,0x0A,0x00,0x00,0x35,0x0A,0x00,0x00,0x00,0x00,0x00,0x00}},
{0x00,1,{0x80}},
{0xCB,16,{0x00,0x05,0x00,0x00,0x05,0x05,0x00,0x05,0x0A,0xC0,0xC5,0x00,0x0F,0x00,0x00,0x00}},
{0x00,1,{0x90}},
{0xCB,16,{0x00,0x04,0x00,0x00,0x04,0x04,0x00,0x04,0x08,0x00,0x04,0x00,0x0C,0x00,0x00,0x00}},
{0x00,1,{0xA0}},
{0xCB,8,{0x00,0x00,0x00,0x00,0x00,0x00,0x0C,0x00}},
{0x00,1,{0xB0}},
{0xCB,4,{0x10,0x51,0x84,0xC0}},
{0x00,1,{0xC0}},
{0xCB,4,{0x10,0x51,0x84,0xC0}},
{0x00,1,{0xD5}},
{0xCB,11,{0x81,0x00,0x81,0x81,0x00,0x81,0x81,0x00,0x81,0x81,0x00}},
{0x00,1,{0xE0}},
{0xCB,13,{0x81,0x81,0x00,0x81,0x81,0x00,0x81,0x81,0x00,0x81,0x81,0x00,0x81}},
{0x00,1,{0x80}},
{0xCC,16,{0x16,0x16,0x17,0x17,0x18,0x18,0x26,0x1F,0x1F,0x26,0x1E,0x26,0x00,0x20,0x03,0x26}},
{0x00,1,{0x90}},
{0xCC,8,{0x07,0x09,0x0B,0x0D,0x26,0x1C,0x26,0x1E}},
{0x00,1,{0x80}},
{0xCD,16,{0x16,0x16,0x17,0x17,0x18,0x18,0x26,0x1F,0x1F,0x26,0x1E,0x26,0x00,0x20,0x02,0x26}},
{0x00,1,{0x90}},
{0xCD,8,{0x06,0x08,0x0A,0x0C,0x26,0x1C,0x26,0x1E}},
{0x00,1,{0xA0}},
{0xCC,16,{0x16,0x16,0x17,0x17,0x18,0x18,0x26,0x1F,0x1F,0x26,0x1E,0x26,0x00,0x20,0x02,0x26}},
{0x00,1,{0xB0}},
{0xCC,8,{0x08,0x06,0x0C,0x0A,0x26,0x1E,0x26,0x1C}},
{0x00,1,{0xA0}},
{0xCD,16,{0x16,0x16,0x17,0x17,0x18,0x18,0x26,0x1F,0x1F,0x26,0x1E,0x26,0x00,0x20,0x03,0x26}},
{0x00,1,{0xB0}},
{0xCD,8,{0x09,0x07,0x0D,0x0B,0x26,0x1E,0x26,0x1C}},
{0x00,1,{0x86}},
{0xC0,6,{0x01,0x02,0x01,0x01,0x0F,0x05}},
{0x00,1,{0x96}},
{0xC0,6,{0x01,0x02,0x01,0x01,0x0F,0x05}},
{0x00,1,{0xA3}},
{0xCE,6,{0x01,0x02,0x01,0x01,0x1E,0x0B}},
{0x00,1,{0xB3}},
{0xCE,6,{0x01,0x02,0x01,0x01,0x1E,0x0B}},
{0x00,1,{0x66}},
{0xC0,6,{0x01,0x02,0x01,0x01,0x1E,0x0B}},
{0x00,1,{0x93}},
{0xC5,1,{0x37}},
{0x00,1,{0x97}},
{0xC5,1,{0x37}},
{0x00,1,{0x9A}},
{0xC5,1,{0x19}},
{0x00,1,{0x9C}},
{0xC5,1,{0x19}},
{0x00,1,{0xB6}},
{0xC5,8,{0x19,0x19,0x0A,0x0A,0x19,0x19,0x0A,0x0A}},
{0x00,1,{0x99}},
{0xCF,1,{0x50}},
{0x00,1,{0x9C}},
{0xF5,1,{0x00}},
{0x00,1,{0x9E}},
{0xF5,1,{0x00}},
{0x00,1,{0xB0}},
{0xC5,6,{0xD0,0x4A,0x3D,0xD0,0x4A,0x0F}},
{0x00,1,{0x80}},
{0xC5,1,{0x85}},
{0x00,1,{0x00}},
{0xD8,2,{0x2B,0x2B}},
{0x00,1,{0x88}},
{0xC4,1,{0x08}},
{0x00,1,{0x82}},
{0xA7,2,{0x22,0x02}},
{0x00,1,{0x8D}},
{0xA7,3,{0x02,0x00,0x02}},
{0x00,1,{0xA4}},
{0xD7,1,{0x5F}},
{0x00,1,{0x9B}},
{0xC4,1,{0xFF}},
{0x00,1,{0x94}},
{0xE9,1,{0x00}},
{0x00,1,{0x9A}},
{0xC4,1,{0x11}},
{0x00,1,{0x95}},
{0xE9,1,{0x10}},
{0x00,1,{0xB1}},
{0xF5,1,{0x1F}},
{0x00,1,{0x80}},
{0xB3,1,{0x22}},
{0x00,1,{0xB0}},
{0xB3,1,{0x00}},
{0x00,1,{0x83}},
{0xB0,1,{0x63}},
{0x00,1,{0x84}},
{0xA4,1,{0x02}},
{0x00,1,{0x81}},
{0xA6,1,{0x04}},
{0x00,1,{0x8C}},
{0xC3,3,{0x33,0x33,0x33}},
{0x00,1,{0x00}},
{0xE1,40,{0x05,0x06,0x09,0x0F,0x5B,0x19,0x21,0x28,0x32,0xE1,0x39,0x40,0x46,0x4B,0xF9,0x4F,0x57,0x5E,0x65,0xFE,0x6C,0x73,0x7A,0x82,0x05,0x8B,0x90,0x96,0x9D,0x7F,0xA5,0xAF,0xBD,0xC4,0xF3,0xCF,0xDD,0xE6,0xEA,0xE3}},
{0x00,1,{0x30}},
{0xE1,40,{0x05,0x06,0x09,0x0F,0x5B,0x19,0x21,0x28,0x32,0xE0,0x39,0x40,0x46,0x4B,0xF5,0x4F,0x57,0x5E,0x65,0xFE,0x6C,0x73,0x7A,0x82,0x00,0x8B,0x90,0x96,0x9D,0x6E,0xA5,0xAF,0xBD,0xC4,0xF3,0xD0,0xDD,0xE6,0xEA,0x23}},
{0x00,1,{0x60}},
{0xE1,40,{0x05,0x06,0x09,0x10,0x5D,0x1A,0x22,0x29,0x33,0xE1,0x3B,0x42,0x47,0x4D,0x4C,0x51,0x59,0x60,0x67,0xBF,0x6E,0x75,0x7D,0x85,0xA1,0x8E,0x94,0x9A,0xA2,0xEE,0xAB,0xB5,0xC3,0xCD,0x39,0xD8,0xE9,0xF6,0xFF,0xAF}},
{0x00,1,{0x90}},
{0xE1,40,{0x05,0x06,0x09,0x10,0x5D,0x1A,0x22,0x29,0x33,0xE1,0x3B,0x42,0x47,0x4C,0x4F,0x51,0x59,0x61,0x67,0xA3,0x6E,0x75,0x7C,0x85,0xAC,0x8E,0x94,0x9A,0xA2,0xED,0xAB,0xB5,0xC3,0xCD,0x39,0xD8,0xE9,0xF6,0xFF,0xAF}},
{0x00,1,{0xC0}},
{0xE1,40,{0x05,0x06,0x09,0x0F,0x5B,0x19,0x21,0x27,0x32,0x9C,0x3A,0x41,0x46,0x4B,0x0F,0x50,0x58,0x5F,0x66,0x54,0x6C,0x73,0x7A,0x82,0x94,0x8A,0x90,0x95,0x9C,0xC9,0xA3,0xAC,0xB9,0xC0,0xF0,0xC9,0xD4,0xDB,0xDF,0x26}},
{0x00,1,{0xF0}},
{0xE1,16,{0x05,0x06,0x09,0x0F,0x5B,0x19,0x21,0x27,0x31,0x9F,0x3A,0x41,0x46,0x4B,0x0F,0x50}},
{0x00,1,{0x00}},
{0xE2,24,{0x58,0x5F,0x66,0x54,0x6C,0x73,0x7A,0x81,0x93,0x8A,0x8F,0x95,0x9C,0xF8,0xA3,0xAC,0xB9,0xC0,0xF0,0xC9,0xD4,0xDB,0xDF,0x66}},
{0x00,1,{0x00}},
{0xE3,40,{0x05,0x06,0x09,0x0F,0x5B,0x19,0x21,0x28,0x32,0xE1,0x39,0x40,0x46,0x4B,0xF9,0x4F,0x57,0x5E,0x65,0xFE,0x6C,0x73,0x7A,0x82,0x05,0x8B,0x90,0x96,0x9D,0x7F,0xA5,0xAF,0xBD,0xC4,0xF3,0xCF,0xDD,0xE6,0xEA,0xE3}},
{0x00,1,{0x30}},
{0xE3,40,{0x05,0x06,0x09,0x0F,0x5B,0x19,0x21,0x28,0x32,0xE0,0x39,0x40,0x46,0x4B,0xF5,0x4F,0x57,0x5E,0x65,0xFE,0x6C,0x73,0x7A,0x82,0x00,0x8B,0x90,0x96,0x9D,0x6E,0xA5,0xAF,0xBD,0xC4,0xF3,0xD0,0xDD,0xE6,0xEA,0x23}},
{0x00,1,{0x60}},
{0xE3,40,{0x05,0x06,0x09,0x10,0x5D,0x1A,0x22,0x29,0x33,0xE1,0x3B,0x42,0x47,0x4D,0x4C,0x51,0x59,0x60,0x67,0xBF,0x6E,0x75,0x7D,0x85,0xA1,0x8E,0x94,0x9A,0xA2,0xEE,0xAB,0xB5,0xC3,0xCD,0x39,0xD8,0xE9,0xF6,0xFF,0xAF}},
{0x00,1,{0x90}},
{0xE3,40,{0x05,0x06,0x09,0x10,0x5D,0x1A,0x22,0x29,0x33,0xE1,0x3B,0x42,0x47,0x4C,0x4F,0x51,0x59,0x61,0x67,0xA3,0x6E,0x75,0x7C,0x85,0xAC,0x8E,0x94,0x9A,0xA2,0xED,0xAB,0xB5,0xC3,0xCD,0x39,0xD8,0xE9,0xF6,0xFF,0xAF}},
{0x00,1,{0xC0}},
{0xE3,40,{0x05,0x06,0x09,0x0F,0x5B,0x19,0x21,0x27,0x32,0x9C,0x3A,0x41,0x46,0x4B,0x0F,0x50,0x58,0x5F,0x66,0x54,0x6C,0x73,0x7A,0x82,0x94,0x8A,0x90,0x95,0x9C,0xC9,0xA3,0xAC,0xB9,0xC0,0xF0,0xC9,0xD4,0xDB,0xDF,0x26}},
{0x00,1,{0xF0}},
{0xE3,16,{0x05,0x06,0x09,0x0F,0x5B,0x19,0x21,0x27,0x31,0x9F,0x3A,0x41,0x46,0x4B,0x0F,0x50}},
{0x00,1,{0x00}},
{0xE4,24,{0x58,0x5F,0x66,0x54,0x6C,0x73,0x7A,0x81,0x93,0x8A,0x8F,0x95,0x9C,0xF8,0xA3,0xAC,0xB9,0xC0,0xF0,0xC9,0xD4,0xDB,0xDF,0x66}},
{0x00,1,{0x82}},
{0xF5,1,{0x01}},
{0x00,1,{0x93}},
{0xF5,1,{0x01}},
{0x00,1,{0x9B}},
{0xF5,1,{0x49}},
{0x00,1,{0x9D}},
{0xF5,1,{0x49}},
{0x00,1,{0xBE}},
{0xC5,2,{0xF0,0xF0}},
{0x00,1,{0xE8}},
{0xC0,1,{0x40}},
{0x00,1,{0x80}},
{0xA7,1,{0x03}},
{0x00,1,{0xCC}},
{0xC0,1,{0x13}},
{0x00,1,{0x00}},
{0xD0,1,{0x0A}},
{0x00,1,{0xE0}},
{0xCF,1,{0x34}},
{0x00,1,{0x92}},
{0xC5,1,{0x00}},
{0x00,1,{0xA0}},
{0xC5,1,{0x40}},
{0x00,1,{0x98}},
{0xC5,1,{0x27}},
{0x00,1,{0xA1}},
{0xC5,1,{0x40}},
{0x00,1,{0x9D}},
{0xC5,1,{0x44}},
{0x00,1,{0x94}},
{0xC5,1,{0x04}},
{0x00,1,{0xB0}},
{0xCA,3,{0x01,0x01,0x0C}},
{0x00,1,{0xB5}},
{0xCA,1,{0x08}},

{0x00,1,{0xA9}},
{0xF6,1,{0x01}},
{0x00,1,{0x88}},
{0xF6,1,{0x5A}},
{0x00,1,{0x90}},
{0xF6,8,{0x2C,0xFF,0xFF,0xFF,0x00,0x00,0x00,0x00}},//Bist mode

{0x00,1,{0x00}},
{0xFF,3,{0xFF,0xFF,0xFF}},
{0x51,2,{0x00,0x00}},
{0x53,1,{0x24}},
{0x55,1,{0x00}},
{0x11, 0, {} },
{REGFLAG_DELAY, 120, {} },
{0x29, 0, {} },
{REGFLAG_DELAY, 10, {} },
};

static struct LCD_setting_table cmd_bl_level[] = {
	{0x51, 3, {0x51, 0x0F, 0xFF} },
	{REGFLAG_END_OF_TABLE, 0x00, {} },
};
