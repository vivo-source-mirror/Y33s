/*
 * TUSB422 Power Delivery
 *
 * Author: Brian Quach <brian.quach@ti.com>
 *
 * Copyright (C) 2016 Texas Instruments Incorporated - http://www.ti.com/
 * 
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as 
 * published by the Free Software Foundation version 2.
 *
 * This program is distributed "as is" WITHOUT ANY WARRANTY of any
 * kind, whether express or implied; without even the implied warranty
 * of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#ifndef __USB_PD_PAL_H__
#define __USB_PD_PAL_H__

#include "tusb422_tcpm.h"
#include "usb_pd_policy_engine.h"

void usb_pd_pal_source_vconn(unsigned int port, bool enable);
void usb_pd_pal_disable_vbus(unsigned int port);
void usb_pd_pal_source_vbus(unsigned int port, bool usb_pd, uint16_t mv, uint16_t ma);
void usb_pd_pal_sink_vbus(unsigned int port, bool usb_pd, uint16_t mv, uint16_t ma);
void usb_pd_pal_sink_vbus_batt(unsigned int port, uint16_t min_mv, uint16_t max_mv, uint16_t mw);
void usb_pd_pal_sink_vbus_vari(unsigned int port, uint16_t min_mv, uint16_t max_mv, uint16_t ma);

void usb_pd_pal_notify_connect_state(unsigned int port, tcpc_state_t state, bool polarity);
void usb_pd_pal_notify_pd_state(unsigned int port, usb_pd_pe_state_t state);

void usb_pd_pal_power_role_swap(unsigned int port, uint8_t new_role);
void usb_pd_pal_data_role_swap(unsigned int port, uint8_t new_role);

/* Custom Type-C headphone support */
#ifdef CONFIG_HIFI_USB
void usb_pd_pal_check_hifi_usb_status(unsigned int port);
#endif

#endif
