// SPDX-License-Identifier: GPL-2.0
/*
 * zte_ecm - ECM data interface driver for the ZTE ZX297510 (MF253S) modem
 *
 * The modem exposes four vendor-specific USB interfaces (if0..if3); interface
 * 1 ("USB-Rndis") is the ECM data path.  It is not a standard CDC ECM
 * interface (class 0xff/0xff/0xff instead of 0x02/0x06), so cdc_ether never
 * binds it, and it has no separate CDC control interface.
 *
 * The ECM descriptors are nevertheless embedded in the interface's
 * class-specific extra descriptors, so the generic CDC data-path plumbing of
 * usbnet (usbnet_generic_cdc_bind, which parses the CDC union and Ethernet
 * functional descriptors and hooks up the bulk endpoints) is enough to turn
 * it into a regular Ethernet netdevice.
 *
 * The interface is matched by vendor/product/class plus the interface number
 * so that the other (AT) interfaces of the same device stay with zte_atfix.
 *
 * Notes:
 *  - the net device carries no DHCP server; the assigned IPv4 settings are
 *    delivered through the modem's +ZGIPDNS URC and applied by the host (see
 *    zte_atfix),
 *  - traffic only starts flowing after the vendor command AT+ZGACT=1,1 has
 *    been issued on an AT interface.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/usb.h>
#include <linux/usb/usbnet.h>

#define ZTE_VENDOR_ID	0x19d2
#define ZTE_PRODUCT_ID	0x0199
#define ZTE_ECM_IFACE	1

/*
 * The interface has the data bulk endpoints plus a CDC interrupt status
 * endpoint, but no CDC union descriptor, so the CDC bind helpers cannot be
 * used.  Plain usbnet endpoint discovery is enough; usbnet_cdc_status
 * (from cdc_ether) handles the status notifications on the interrupt
 * endpoint.
 */
static const struct driver_info zte_ecm_info = {
	.description	= "ZTE ZX297510 (MF253S) ECM data interface",
	.flags		= FLAG_ETHER,
	.status		= usbnet_cdc_status,
	.manage_power	= usbnet_manage_power,
};

static const struct usb_device_id zte_ecm_ids[] = {
	{
		.match_flags	= USB_DEVICE_ID_MATCH_DEVICE |
				  USB_DEVICE_ID_MATCH_INT_INFO |
				  USB_DEVICE_ID_MATCH_INT_NUMBER,
		.idVendor	= ZTE_VENDOR_ID,
		.idProduct	= ZTE_PRODUCT_ID,
		.bInterfaceNumber = ZTE_ECM_IFACE,
		.bInterfaceClass = USB_CLASS_VENDOR_SPEC,
		.bInterfaceSubClass = 0xff,
		.bInterfaceProtocol = 0xff,
		.driver_info	= (kernel_ulong_t)&zte_ecm_info,
	},
	{ }
};
MODULE_DEVICE_TABLE(usb, zte_ecm_ids);

static struct usb_driver zte_ecm_driver = {
	.name		= "zte_ecm",
	.id_table	= zte_ecm_ids,
	.probe		= usbnet_probe,
	.disconnect	= usbnet_disconnect,
	.suspend	= usbnet_suspend,
	.resume		= usbnet_resume,
	.disable_hub_initiated_lpm = 1,
};

module_usb_driver(zte_ecm_driver);

MODULE_DESCRIPTION("ZTE ZX297510 (MF253S) ECM data interface driver");
MODULE_LICENSE("GPL");
