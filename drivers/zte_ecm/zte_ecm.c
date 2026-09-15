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
 *    been issued on an AT interface,
 *  - IPv6 is not supported by the firmware, so this driver simply declares
 *    the interface IPv6-incapable: every IPv6 frame handed to the netdev is
 *    dropped on TX (upper layers may enable IPv6 freely, not a single IPv6
 *    packet ever reaches the modem) and the per-device IPv6 stack is
 *    best-effort disabled so well-behaved userlands skip it entirely.
 */

#include <linux/ipv6.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/usb.h>
#include <linux/usb/usbnet.h>
#include <net/addrconf.h>
#include <net/if_inet6.h>

#define ZTE_VENDOR_ID	0x19d2
#define ZTE_PRODUCT_ID	0x0199
#define ZTE_ECM_IFACE	1

static bool zte_ecm_v6_dropped;

/*
 * The firmware has no IPv6 data path (no RA/NDP handling, no DHCPv6, no
 * global address), so IPv6 frames must never reach the modem.  Drop them
 * here: usbnet then accounts them as tx_dropped.
 */
static struct sk_buff *zte_ecm_tx_fixup(struct usbnet *dev, struct sk_buff *skb,
					gfp_t flags)
{
	if (skb->protocol == htons(ETH_P_IPV6)) {
		if (!zte_ecm_v6_dropped) {
			zte_ecm_v6_dropped = true;
			netdev_info(dev->net,
				    "dropping IPv6 traffic (firmware has no IPv6 data path)\n");
		}
		dev_kfree_skb_any(skb);
		return NULL;
	}
	return skb;
}

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
	.tx_fixup	= zte_ecm_tx_fixup,
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

/*
 * IPv6 is unusable on this modem (no RA/NDP, no DHCPv6, no global address),
 * but both ModemManager and NetworkManager keep probing/announcing it, which
 * only delays connections.  Switch it off per device as soon as the netdev
 * appears, so every userland sees the interface as IPv6-disabled.
 *
 * The notifier is registered before the USB driver, and the addrconf
 * notifier (registered at boot) runs first, so the inet6_dev exists by the
 * time we get the registration event.
 */
static int zte_ecm_netdev_event(struct notifier_block *nb,
				unsigned long event, void *ptr)
{
	struct net_device *dev = netdev_notifier_info_to_dev(ptr);
	struct inet6_dev *idev;

	if (event != NETDEV_REGISTER && event != NETDEV_UP)
		return NOTIFY_DONE;

	/* match only the netdevs created by this driver */
	if (!dev->dev.parent ||
	    dev->dev.parent->driver != &zte_ecm_driver.driver)
		return NOTIFY_DONE;

	idev = __in6_dev_get(dev);
	if (idev && !idev->cnf.disable_ipv6) {
		idev->cnf.disable_ipv6 = 1;
		pr_info("zte_ecm: IPv6 disabled on %s (firmware has no IPv6 data path)\n",
			dev->name);
	}

	return NOTIFY_OK;
}

static struct notifier_block zte_ecm_netdev_nb = {
	.notifier_call = zte_ecm_netdev_event,
};

/*
 * qmi_wwan also matches this interface and would crash the firmware with
 * QMI probing; option may grab the other interfaces.  Take our interface
 * over deterministically: release any competing driver and force-attach
 * ours (USB devices do not support the driver core's driver_override, so
 * device_driver_attach is used to bypass the probe-order race).
 */
static struct work_struct zte_ecm_claim_work;
static bool zte_ecm_claim_attach;

/* not declared in the installed headers */
extern int device_driver_attach(const struct device_driver *drv,
				struct device *dev);

static int zte_ecm_claim_walk(struct usb_device *udev, void *data)
{
	struct usb_interface *intf;
	struct device *dev;
	int i;

	if (!udev->actconfig)
		return 0;
	if (le16_to_cpu(udev->descriptor.idVendor) != ZTE_VENDOR_ID ||
	    le16_to_cpu(udev->descriptor.idProduct) != ZTE_PRODUCT_ID)
		return 0;

	for (i = 0; i < udev->actconfig->desc.bNumInterfaces; i++) {
		intf = udev->actconfig->interface[i];
		if (!intf || !intf->cur_altsetting)
			continue;
		if (intf->cur_altsetting->desc.bInterfaceNumber != ZTE_ECM_IFACE)
			continue;

		dev = &intf->dev;
		if (dev->driver && strcmp(dev->driver->name, "zte_ecm")) {
			dev_info(dev, "zte_ecm: releasing competing driver '%s'\n",
				 dev->driver->name);
			device_release_driver(dev);
		}
		if (zte_ecm_claim_attach && !dev->driver)
			device_driver_attach(&zte_ecm_driver.driver, dev);
	}
	return 0;
}

static void zte_ecm_claim_work_fn(struct work_struct *work)
{
	zte_ecm_claim_attach = true;
	usb_for_each_dev(NULL, zte_ecm_claim_walk);
	zte_ecm_claim_attach = false;
}

static struct delayed_work zte_ecm_claim_watch_work;

static void zte_ecm_claim_watch_fn(struct work_struct *work)
{
	zte_ecm_claim_work_fn(work);
	queue_delayed_work(system_wq, &zte_ecm_claim_watch_work,
			   msecs_to_jiffies(1000));
}

static int zte_ecm_usb_notify(struct notifier_block *nb, unsigned long action,
			      void *data)
{
	struct usb_device *udev = data;

	if (action != USB_DEVICE_ADD)
		return NOTIFY_OK;
	if (le16_to_cpu(udev->descriptor.idVendor) != ZTE_VENDOR_ID ||
	    le16_to_cpu(udev->descriptor.idProduct) != ZTE_PRODUCT_ID)
		return NOTIFY_OK;

	schedule_work(&zte_ecm_claim_work);
	return NOTIFY_OK;
}

static struct notifier_block zte_ecm_usb_nb = {
	.notifier_call = zte_ecm_usb_notify,
};

static int __init zte_ecm_init(void)
{
	int ret;

	ret = register_netdevice_notifier(&zte_ecm_netdev_nb);
	if (ret)
		return ret;

	INIT_WORK(&zte_ecm_claim_work, zte_ecm_claim_work_fn);
	INIT_DELAYED_WORK(&zte_ecm_claim_watch_work, zte_ecm_claim_watch_fn);

	/* release whoever grabbed the interface before us */
	usb_for_each_dev(NULL, zte_ecm_claim_walk);
	usb_register_notify(&zte_ecm_usb_nb);

	ret = usb_register(&zte_ecm_driver);
	if (ret) {
		usb_unregister_notify(&zte_ecm_usb_nb);
		cancel_work_sync(&zte_ecm_claim_work);
		unregister_netdevice_notifier(&zte_ecm_netdev_nb);
		return ret;
	}

	queue_delayed_work(system_wq, &zte_ecm_claim_watch_work,
			   msecs_to_jiffies(1000));
	return 0;
}

static void __exit zte_ecm_exit(void)
{
	usb_unregister_notify(&zte_ecm_usb_nb);
	cancel_work_sync(&zte_ecm_claim_work);
	cancel_delayed_work_sync(&zte_ecm_claim_watch_work);
	usb_deregister(&zte_ecm_driver);
	unregister_netdevice_notifier(&zte_ecm_netdev_nb);
}

module_init(zte_ecm_init);
module_exit(zte_ecm_exit);

MODULE_DESCRIPTION("ZTE ZX297510 (MF253S) ECM data interface driver");
MODULE_LICENSE("GPL");
