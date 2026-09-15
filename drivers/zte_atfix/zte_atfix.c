// SPDX-License-Identifier: GPL-2.0
/*
 * zte_atfix - AT fixup + link management driver for the ZTE ZX297510
 * (MF253S / ME3760V2) modem
 *
 * The ZX297510 firmware's AT parser rejects a number of standard commands
 * (all answer "+CME ERROR: 6003"), and it does not implement the dial
 * sequence in a way any standard host stack understands:
 *
 *  - no ATD*99 PPP (rejected with +CME ERROR: 3 once the always-on LTE
 *    default bearer is up),
 *  - the ECM data path only carries traffic after the vendor command
 *    AT+ZGACT=1,1 has been issued,
 *  - the assigned IPv4 address/gateway/DNS are only delivered through the
 *    +ZGIPDNS unsolicited message, there is no DHCP on the net port.
 *
 * This driver makes the modem usable by stock ModemManager / NetworkManager
 * installations without any userspace helper:
 *
 *  1) command fixups on the AT interfaces (0, 2, 3):
 *       ATZ\r          -> rewritten to "ATE0\r"
 *       AT+WS46=?\r    -> synthetic "+WS46: (28)" + OK
 *       AT+WS46=<n>\r  -> synthetic OK
 *       AT+GCAP\r      -> synthetic "+GCAP: +CGSM,+CLTE" + OK
 *       AT%IPSYS?\r    -> synthetic "%IPSYS: 0,1,0" + OK (Icera detection)
 *     so that ModemManager enables LTE (EPS) tracking and picks its Icera
 *     modem/bearer implementation, which knows how to use a net data port.
 *
 *  2) Icera bearer emulation for the dial/IP steps ModemManager issues:
 *       AT%IPDPACT=<cid>,1  -> perform the real dial on a private AT port
 *                              (AT+CGACT=1,1 + AT+ZGACT=1,1), then answer
 *                              OK plus a "%IPDPACT: <cid>,1,0" URC
 *       AT%IPDPACT=<cid>,0  -> answer OK plus a "%IPDPACT: <cid>,0,0" URC
 *       AT%IPDPADDR=<cid>   -> answer with the IPv4 settings learned from
 *                              +ZGIPDNS (static IP config for the bearer)
 *
 *  3) link management on a private AT interface (2) that is hidden from
 *     ModemManager: the CPE-derived init sequence, continuous polling (the
 *     modem resets after ~21 s when the host stops polling it) and the
 *     +ZGIPDNS sniffer that keeps the IP settings cache up to date.
 *
 * The private interface is hidden by dropping bare "AT" probes, which makes
 * ModemManager classify it as a non-AT port; all of the driver's own traffic
 * bypasses the tty layer and uses the bulk endpoint directly.
 */

#include <linux/completion.h>
#include <linux/inet.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/tty.h>
#include <linux/tty_flip.h>
#include <linux/usb.h>
#include <linux/usb/serial.h>
#include <linux/workqueue.h>

#define ZTE_VID 0x19d2
#define ZTE_PID 0x0199

#define ZTE_MAX_PENDING	32

#define ZTE_IF_AT	0
#define ZTE_IF_DATA	1
#define ZTE_IF_MODEM	2
#define ZTE_IF_LOG	3

/*
 * Match the ZTE AT interfaces by number:
 *   0 = USB-AT, 2 = USB-Modem, 3 = USB-log
 * (interface 1 = USB-Rndis data is owned by zte_ecm)
 */
#define ZTE_AT_IFACE(num) {						\
	.match_flags = USB_DEVICE_ID_MATCH_DEVICE |			\
		       USB_DEVICE_ID_MATCH_INT_INFO |			\
		       USB_DEVICE_ID_MATCH_INT_NUMBER,			\
	.idVendor = ZTE_VID,						\
	.idProduct = ZTE_PID,						\
	.bInterfaceNumber = (num),					\
	.bInterfaceClass = USB_CLASS_VENDOR_SPEC,			\
	.bInterfaceSubClass = 0xff,					\
	.bInterfaceProtocol = 0xff,					\
}

static const struct usb_device_id zte_atfix_ids[] = {
	ZTE_AT_IFACE(ZTE_IF_AT),
	ZTE_AT_IFACE(ZTE_IF_MODEM),
	ZTE_AT_IFACE(ZTE_IF_LOG),
	{ }
};
MODULE_DEVICE_TABLE(usb, zte_atfix_ids);

static const char * const zte_patterns[] = {
	"ATZ",
	"AT+GCAP",
	"AT+WS46=?",
	"AT%IPSYS?",
	"AT%NWSTATE",
	"AT+CSQ",
	"AT+CEREG?",
};

#define ZTE_VAR_IPDPADDR	"AT%IPDPADDR="
#define ZTE_VAR_IPDPACT		"AT%IPDPACT="
#define ZTE_VAR_CEREG		"AT+CEREG="
#define ZTE_VAR_IPSYS		"AT%IPSYS="

/*
 * This firmware reports a non-standard CSQ value: for valid readings the
 * "rssi" byte is 253 + RSRP(dBm) (e.g. 149 -> RSRP -104 dBm), while 0/99/100
 * mark an unknown value.  ModemManager expects the standard 0..31 RSSI
 * scale (RSSI -113..-51 dBm, 2 dBm per unit), so convert RSRP into an
 * approximate RSSI first (adding the typical LTE bandwidth offset) and then
 * apply the standard mapping.
 */
#define ZTE_CSQ_RSRP_BASE	253
#define ZTE_CSQ_RSRP_TO_RSSI	25
#define ZTE_ACT_LTE		7

/* Technology string reported in the faked %NWSTATE response.  MM's Icera
 * parser only knows 2G/3G strings (there is no LTE case), so this is a
 * module parameter to allow picking the closest one at runtime. */
static char *zte_nwstate_tech = "HSDPA-HSUPA-HSPA+";
module_param_named(nwstate_tech, zte_nwstate_tech, charp, 0644);
MODULE_PARM_DESC(nwstate_tech, "Technology string for the faked %NWSTATE");

static bool zte_nwstate_ok = true;
module_param_named(nwstate_ok, zte_nwstate_ok, bool, 0644);
MODULE_PARM_DESC(nwstate_ok, "Answer %NWSTATE or report it unsupported (CME 4)");

/* values sniffed from the modem's own responses */
static int zte_csq_raw = -1;
static int zte_cereg_n;
static int zte_cereg_stat = -1;

static int zte_csq_normalize(int raw)
{
	int rssi;

	if (raw <= 0 || raw == 99 || raw == 100 || raw > 191)
		return -1;
	rssi = raw - ZTE_CSQ_RSRP_BASE + ZTE_CSQ_RSRP_TO_RSSI;
	if (rssi <= -113)
		return 0;
	if (rssi >= -51)
		return 31;
	return (rssi + 113) / 2;
}

/* per-port command-fixup state */
struct zte_atfix_port {
	u8	pending[ZTE_MAX_PENDING];
	int	pending_len;
	bool	in_ws46_set;
	bool	quiet;
	int	ifnum;
	bool	logged_atz;
	bool	logged_gcap;
	bool	logged_ws46;
	bool	logged_ipsys;
};

/* IP configuration learned from the +ZGIPDNS unsolicited message */
struct zte_ip_cache {
	spinlock_t	lock;
	bool		valid;
	char		ip[16];
	char		gw[16];
	char		dns1[16];
	char		dns2[16];
};

static struct zte_ip_cache zte_cache;

/* private command channel (interface 2) */
static struct usb_serial_port *zte_cmd_port;
static struct usb_serial_port *zte_at_port;
static DEFINE_SPINLOCK(zte_cmd_lock);
static struct completion zte_cmd_done;
static bool zte_cmd_pending;
static char zte_cmd_resp[512];
static int zte_cmd_resp_len;
static char *zte_cmd_buf;

/* private read channel (interface 2 is never opened by userspace) */
#define ZTE_PRIV_BUF_SIZE 512
static struct urb *zte_priv_urb;
static u8 *zte_priv_buf;

/* deferred work */
static struct workqueue_struct *zte_wq;
static struct work_struct zte_init_work;
static struct delayed_work zte_poll_work;
static unsigned int zte_poll_idx;
static struct work_struct zte_dial_work;
static DEFINE_SPINLOCK(zte_dial_lock);
static struct usb_serial_port *zte_dial_port;
static unsigned int zte_dial_cid;
static bool zte_dial_queued;

static const char * const zte_poll_cmds[] = {
	"AT+CSQ\r",
	"AT^SYSINFO\r",
	"AT+CEREG?\r",
	"AT+CREG?\r",
	"AT+CGREG?\r",
	"AT+COPS=3,2\r",
	"AT+COPS?\r",
};

static void zte_atfix_inject(struct usb_serial_port *port, const char *str);
static void zte_atfix_forward(struct tty_struct *tty,
			      struct usb_serial_port *port,
			      const u8 *data, int len);
static void zte_cache_zgipdns(const u8 *data, unsigned int len);
static void zte_sniff_rx(const u8 *data, unsigned int len);

static int zte_ifnum(struct usb_serial_port *port)
{
	return port->serial->interface->cur_altsetting->desc.bInterfaceNumber;
}

/* ---------------------------------------------------------------------- */
/* private command channel                                                */

static int zte_send_cmd(const char *cmd, char *resp, size_t respsz,
			unsigned long timeout_ms)
{
	struct usb_serial_port *port;
	struct usb_device *udev;
	unsigned long flags;
	int actual = 0;
	int ret;

	spin_lock_irqsave(&zte_cmd_lock, flags);
	port = zte_cmd_port;
	if (!port || !port->serial || !zte_cmd_buf) {
		spin_unlock_irqrestore(&zte_cmd_lock, flags);
		return -ENODEV;
	}
	strscpy(zte_cmd_buf, cmd, 256);
	zte_cmd_resp_len = 0;
	zte_cmd_resp[0] = '\0';
	reinit_completion(&zte_cmd_done);
	zte_cmd_pending = true;
	spin_unlock_irqrestore(&zte_cmd_lock, flags);

	udev = port->serial->dev;
	ret = usb_bulk_msg(udev,
			   usb_sndbulkpipe(udev, port->bulk_out_endpointAddress),
			   zte_cmd_buf, strlen(zte_cmd_buf), &actual, 1000);
	if (ret) {
		spin_lock_irqsave(&zte_cmd_lock, flags);
		zte_cmd_pending = false;
		spin_unlock_irqrestore(&zte_cmd_lock, flags);
		return ret;
	}

	if (!wait_for_completion_timeout(&zte_cmd_done,
					 msecs_to_jiffies(timeout_ms))) {
		spin_lock_irqsave(&zte_cmd_lock, flags);
		zte_cmd_pending = false;
		spin_unlock_irqrestore(&zte_cmd_lock, flags);
		return -ETIMEDOUT;
	}

	if (resp) {
		spin_lock_irqsave(&zte_cmd_lock, flags);
		strscpy(resp, zte_cmd_resp, respsz);
		spin_unlock_irqrestore(&zte_cmd_lock, flags);
	}
	return 0;
}

static bool zte_cmd_response_ok(const char *resp)
{
	return resp && strstr(resp, "OK") && !strstr(resp, "ERROR");
}

static void zte_priv_read_cb(struct urb *urb)
{
	struct usb_serial_port *port = urb->context;
	unsigned long flags;
	bool done = false;

	if (urb->status) {
		if (urb->status != -ESHUTDOWN && urb->status != -ENOENT &&
		    urb->status != -ENODEV)
			usb_submit_urb(urb, GFP_ATOMIC);
		return;
	}

	if (urb->actual_length) {
		bool complete = false;

		zte_sniff_rx(urb->transfer_buffer, urb->actual_length);

		spin_lock_irqsave(&zte_cmd_lock, flags);
		if (zte_cmd_pending && zte_cmd_port == port) {
			int n = min_t(int, urb->actual_length,
				      (int)sizeof(zte_cmd_resp) - 1 -
				      zte_cmd_resp_len);

			if (n > 0) {
				memcpy(zte_cmd_resp + zte_cmd_resp_len,
				       urb->transfer_buffer, n);
				zte_cmd_resp_len += n;
				zte_cmd_resp[zte_cmd_resp_len] = '\0';
			}
			if (strstr(zte_cmd_resp, "\r\nOK\r\n") ||
			    strstr(zte_cmd_resp, "ERROR")) {
				zte_cmd_pending = false;
				complete = true;
			}
		}
		spin_unlock_irqrestore(&zte_cmd_lock, flags);
		done = complete;
	}

	if (done)
		complete(&zte_cmd_done);
	usb_submit_urb(urb, GFP_ATOMIC);
}

/* ---------------------------------------------------------------------- */
/* +ZGIPDNS cache                                                         */

static int zte_next_quoted(const char **pp, const char *end,
			   char *out, size_t outsz)
{
	const char *p = *pp;
	const char *q;

	while (p < end && *p != '"')
		p++;
	if (p >= end)
		return -1;
	p++;
	q = p;
	while (q < end && *q != '"')
		q++;
	if (q >= end)
		return -1;
	strscpy(out, p, min((size_t)(q - p) + 1, outsz));
	*pp = q + 1;
	return 0;
}

static const char *zte_memmem(const u8 *hay, size_t haylen, const char *needle)
{
	size_t nlen = strlen(needle);
	size_t i;

	if (haylen < nlen)
		return NULL;
	for (i = 0; i <= haylen - nlen; i++) {
		if (!memcmp(hay + i, needle, nlen))
			return (const char *)(hay + i);
	}
	return NULL;
}

/* cache the IP settings carried by +ZGIPDNS: <n>,<n>,"IP","<ip>","<gw>","<d1>","<d2>" */
static void zte_cache_zgipdns(const u8 *data, unsigned int len)
{
	const char *tag = "+ZGIPDNS:";
	const char *end = (const char *)data + len;
	const char *p;
	char type[8], ip[16], gw[16], dns1[16], dns2[16];
	unsigned long flags;

	p = zte_memmem(data, len, tag);
	if (!p)
		return;
	p += strlen(tag);

	if (zte_next_quoted(&p, end, type, sizeof(type)) ||
	    zte_next_quoted(&p, end, ip, sizeof(ip)) ||
	    zte_next_quoted(&p, end, gw, sizeof(gw)) ||
	    zte_next_quoted(&p, end, dns1, sizeof(dns1)))
		return;
	if (zte_next_quoted(&p, end, dns2, sizeof(dns2)))
		dns2[0] = '\0';

	if (strcmp(type, "IP"))
		return;

	spin_lock_irqsave(&zte_cache.lock, flags);
	strscpy(zte_cache.ip, ip, sizeof(zte_cache.ip));
	strscpy(zte_cache.gw, gw, sizeof(zte_cache.gw));
	strscpy(zte_cache.dns1, dns1, sizeof(zte_cache.dns1));
	strscpy(zte_cache.dns2, dns2, sizeof(zte_cache.dns2));
	zte_cache.valid = true;
	spin_unlock_irqrestore(&zte_cache.lock, flags);
}

/* sniff "+CSQ: <raw>,<ber>" responses (any port) */
static void zte_sniff_csq(const u8 *data, unsigned int len)
{
	const char *p = zte_memmem(data, len, "+CSQ:");

	if (p) {
		int raw = -1;

		if (sscanf(p, "+CSQ: %d", &raw) == 1 && raw >= 0)
			WRITE_ONCE(zte_csq_raw, raw);
	}
}

/* sniff "+CEREG: <n>,<stat>..." responses (any port) */
static void zte_sniff_cereg(const u8 *data, unsigned int len)
{
	const char *p = zte_memmem(data, len, "+CEREG:");

	if (p) {
		int n = 0, stat = -1;

		if (sscanf(p, "+CEREG: %d,%d", &n, &stat) == 2 && stat >= 0) {
			WRITE_ONCE(zte_cereg_n, n);
			WRITE_ONCE(zte_cereg_stat, stat);
		}
	}
}

static void zte_sniff_rx(const u8 *data, unsigned int len)
{
	zte_cache_zgipdns(data, len);
	zte_sniff_csq(data, len);
	zte_sniff_cereg(data, len);
}

/* MM expects the standard 0..31 CSQ range; also give it the LTE AcT that
 * this firmware omits from +CEREG. */
/* 0..5 signal level used both by the CSQ spoof and the %NWSTATE spoof, so
 * MM sees one consistent value no matter which source it reads */
static int zte_signal_level(void)
{
	int csq = zte_csq_normalize(READ_ONCE(zte_csq_raw));

	if (csq < 0)
		return -1;
	return (csq * 5 + 15) / 31;
}

static void zte_answer_csq(struct usb_serial_port *port)
{
	int lvl = zte_signal_level();
	int csq = (lvl >= 0) ? lvl * 31 / 5 : -1;
	char resp[48];

	if (csq < 0)
		snprintf(resp, sizeof(resp), "\r\n+CSQ: 99,99\r\n\r\nOK\r\n");
	else
		snprintf(resp, sizeof(resp),
			 "\r\n+CSQ: %d,99\r\n\r\nOK\r\n", csq);
	dev_info(&port->dev, "answering AT+CSQ (raw %d -> level %d -> %d)\n",
		 READ_ONCE(zte_csq_raw), lvl, csq);
	zte_atfix_inject(port, resp);
}

static void zte_answer_cereg(struct usb_serial_port *port)
{
	char resp[64];

	snprintf(resp, sizeof(resp),
		 "\r\n+CEREG: %d,%d,,,%d\r\n\r\nOK\r\n",
		 READ_ONCE(zte_cereg_n), READ_ONCE(zte_cereg_stat),
		 ZTE_ACT_LTE);
	dev_info(&port->dev, "answering AT+CEREG? (LTE act)\n");
	zte_atfix_inject(port, resp);
}

/*
 * Answer MM's Icera access-technology query.  The mapping in MM only knows
 * 2G/3G strings, so the reported technology is configurable (see the
 * nwstate_tech module parameter).
 */
static void zte_answer_nwstate(struct usb_serial_port *port)
{
	int lvl = zte_signal_level();
	int rssi = (lvl >= 0) ? lvl : 3;
	char resp[128];

	if (!zte_nwstate_ok) {
		/* declaring it unsupported makes MM stop polling access
		 * technologies, so the LTE AcT from +CEREG can stick */
		dev_info(&port->dev, "reporting AT%%NWSTATE as unsupported\n");
		zte_atfix_inject(port, "\r\n+CME ERROR: 4\r\n");
		return;
	}

	snprintf(resp, sizeof(resp),
		 "\r\n%%NWSTATE: %d,0,%s,%s,2\r\n\r\nOK\r\n",
		 rssi, zte_nwstate_tech, zte_nwstate_tech);
	dev_info(&port->dev, "answering AT%%NWSTATE (tech %s)\n",
		 zte_nwstate_tech);
	zte_atfix_inject(port, resp);
}

static void zte_augment_cereg_urc(struct usb_serial_port *port,
				  const struct zte_atfix_port *st,
				  const u8 *data, unsigned int len)
{
	char resp[64];

	if (st->ifnum != ZTE_IF_AT)
		return;
	if (READ_ONCE(zte_cereg_stat) < 0)
		return;
	if (!zte_memmem(data, len, "+CEREG:"))
		return;

	snprintf(resp, sizeof(resp), "\r\n+CEREG: %d,%d,,,%d\r\n",
		 READ_ONCE(zte_cereg_n), READ_ONCE(zte_cereg_stat),
		 ZTE_ACT_LTE);
	zte_atfix_inject(port, resp);
}

/*
 * MM's Icera class loads the supported modes from AT%IPSYS=? and can only
 * express 2G/3G combinations (there is no LTE case in its parser).  Report
 * 3G-only so GNOME does not offer 2G or 5G modes; the modem itself stays on
 * LTE regardless.
 */
static void zte_answer_ipsys_modes(struct usb_serial_port *port)
{
	dev_info(&port->dev, "answering AT%%IPSYS=? (3G-only modes)\n");
	zte_atfix_inject(port, "\r\n%IPSYS: (1),(1)\r\n\r\nOK\r\n");
}

/* remember the URC mode MM configures, then pass AT+CEREG=<n> through */
static void zte_cache_cereg_cmd(struct tty_struct *tty,
				struct usb_serial_port *port,
				struct zte_atfix_port *st)
{
	int n;

	if (sscanf(st->pending + strlen(ZTE_VAR_CEREG), "%d", &n) == 1)
		WRITE_ONCE(zte_cereg_n, n);
	zte_atfix_forward(tty, port, st->pending, st->pending_len);
}

static bool zte_cache_get(char *ip, size_t ipsz, char *gw, size_t gwsz,
			  char *dns1, size_t d1sz, char *dns2, size_t d2sz)
{
	unsigned long flags;
	bool valid;

	spin_lock_irqsave(&zte_cache.lock, flags);
	valid = zte_cache.valid;
	if (valid) {
		strscpy(ip, zte_cache.ip, ipsz);
		strscpy(gw, zte_cache.gw, gwsz);
		strscpy(dns1, zte_cache.dns1, d1sz);
		strscpy(dns2, zte_cache.dns2, d2sz);
	}
	spin_unlock_irqrestore(&zte_cache.lock, flags);
	return valid;
}

static bool zte_cache_valid(void)
{
	unsigned long flags;
	bool valid;

	spin_lock_irqsave(&zte_cache.lock, flags);
	valid = zte_cache.valid;
	spin_unlock_irqrestore(&zte_cache.lock, flags);
	return valid;
}

static void zte_derive_gw(const char *ip, char *gw, size_t gwsz)
{
	const char *dot = strrchr(ip, '.');
	unsigned long last;

	if (!dot || !*ip || kstrtoul(dot + 1, 10, &last) || last > 254) {
		strscpy(gw, "0.0.0.0", gwsz);
		return;
	}
	snprintf(gw, gwsz, "%.*s%lu", (int)(dot - ip) + 1, ip, last + 1);
}

static bool zte_valid_ipv4(const char *s)
{
	u8 addr[4];

	return s[0] && in4_pton(s, -1, addr, -1, NULL) == 1 &&
	       memcmp(addr, "\0\0\0\0", 4);
}

/* ---------------------------------------------------------------------- */
/* Icera bearer emulation                                                 */

/* answer %IPDPADDR=<cid> with the cached settings (ModemManager Icera bearer) */
static void zte_answer_ipdpaddr(struct usb_serial_port *port, const char *cmd)
{
	unsigned int cid = 0;
	char ip[16] = "", gw[16] = "", dns1[16] = "", dns2[16] = "";
	char resp[256];

	sscanf(cmd + strlen(ZTE_VAR_IPDPADDR), "%u", &cid);

	zte_cache_get(ip, sizeof(ip), gw, sizeof(gw),
		      dns1, sizeof(dns1), dns2, sizeof(dns2));

	if (!zte_valid_ipv4(ip)) {
		dev_info(&port->dev,
			 "no +ZGIPDNS cache yet, failing %%IPDPADDR\n");
		zte_atfix_inject(port, "\r\n+CME ERROR: 3\r\n");
		return;
	}
	if (!zte_valid_ipv4(dns1))
		strscpy(dns1, "223.5.5.5", sizeof(dns1));
	if (!zte_valid_ipv4(gw))
		zte_derive_gw(ip, gw, sizeof(gw));
	if (!zte_valid_ipv4(dns2))
		strscpy(dns2, "223.6.6.6", sizeof(dns2));

	snprintf(resp, sizeof(resp),
		 "\r\n%%IPDPADDR: %u,%s,%s,%s,%s,0.0.0.0,0.0.0.0,"
		 "0.0.0.0,0.0.0.0,::,::,::,::,::,::,::,::\r\n\r\nOK\r\n",
		 cid, ip, gw, dns1, dns2);
	dev_info(&port->dev, "answering %%IPDPADDR=%u (%s)\n", cid, ip);
	zte_atfix_inject(port, resp);
}

/* translate ModemManager's Icera dial into the vendor dial sequence */
static void zte_dial_work_fn(struct work_struct *work)
{
	struct usb_serial_port *port;
	unsigned int cid;
	char resp[64];
	char buf[64];
	unsigned long flags;
	bool connected = false;
	int i, ret;

	spin_lock_irqsave(&zte_dial_lock, flags);
	port = zte_dial_port;
	cid = zte_dial_cid;
	zte_dial_queued = false;
	spin_unlock_irqrestore(&zte_dial_lock, flags);

	if (!port)
		return;

	dev_info(&port->dev, "dialing: CGACT=1,1 + ZGACT=1,1 (cid %u)\n", cid);

	spin_lock_irqsave(&zte_cache.lock, flags);
	zte_cache.valid = false;
	spin_unlock_irqrestore(&zte_cache.lock, flags);

	snprintf(buf, sizeof(buf), "AT+CGACT=1,%u\r", cid ? cid : 1);
	ret = zte_send_cmd(buf, resp, sizeof(resp), 5000);
	dev_info(&port->dev, "CGACT ret=%d => %s\n", ret, resp);

	for (i = 0; i < 3 && !connected; i++) {
		snprintf(buf, sizeof(buf), "AT+ZGACT=1,%u\r", cid ? cid : 1);
		resp[0] = '\0';
		ret = zte_send_cmd(buf, resp, sizeof(resp), 5000);

		/* a fresh +ZGIPDNS means the data path is up; ZGACT itself
		 * answers +CME ERROR: 4 once the context is already bound */
		if (zte_cache_valid())
			connected = true;
		else if (!ret && zte_cmd_response_ok(resp))
			connected = true;
		else
			msleep(1000);
	}
	dev_info(&port->dev, "ZGACT ret=%d => %s\n", ret, resp);

	if (connected) {
		for (i = 0; i < 10; i++) {
			char ip[16], gw[16], d1[16], d2[16];

			if (zte_cache_get(ip, sizeof(ip), gw, sizeof(gw),
					  d1, sizeof(d1), d2, sizeof(d2)))
				break;
			msleep(500);
		}
		snprintf(resp, sizeof(resp),
			 "\r\nOK\r\n\r\n%%IPDPACT: %u,1,0\r\n",
			 cid ? cid : 1);
		dev_info(&port->dev, "dial done, reporting connected\n");
	} else {
		snprintf(resp, sizeof(resp), "\r\n+CME ERROR: 3\r\n");
		dev_info(&port->dev, "dial failed\n");
	}
	zte_atfix_inject(port, resp);
}

static void zte_queue_dial(struct usb_serial_port *port, unsigned int cid)
{
	unsigned long flags;

	spin_lock_irqsave(&zte_dial_lock, flags);
	zte_dial_port = port;
	zte_dial_cid = cid;
	if (!zte_dial_queued) {
		zte_dial_queued = true;
		queue_work(zte_wq, &zte_dial_work);
	}
	spin_unlock_irqrestore(&zte_dial_lock, flags);
}

/* swallow %IPDPACT=<cid>,<0|1> and report the state change as a URC */
static void zte_answer_ipdpact(struct usb_serial_port *port, const char *cmd)
{
	unsigned int cid = 0, status = 0;
	char resp[64];

	if (sscanf(cmd + strlen(ZTE_VAR_IPDPACT), "%u,%u", &cid, &status) != 2)
		status = 1;

	if (status) {
		zte_queue_dial(port, cid);
		return;
	}

	snprintf(resp, sizeof(resp),
		 "\r\nOK\r\n\r\n%%IPDPACT: %u,0,0\r\n", cid);
	dev_info(&port->dev, "answering %%IPDPACT=%u,0\n", cid);
	zte_atfix_inject(port, resp);
}

static bool zte_var_hold(const u8 *p, int len)
{
	size_t alen = strlen(ZTE_VAR_IPDPADDR);
	size_t clen = strlen(ZTE_VAR_IPDPACT);
	size_t rlen = strlen(ZTE_VAR_CEREG);
	size_t ilen = strlen(ZTE_VAR_IPSYS);

	if ((size_t)len < alen && !memcmp(p, ZTE_VAR_IPDPADDR, len))
		return true;
	if ((size_t)len < clen && !memcmp(p, ZTE_VAR_IPDPACT, len))
		return true;
	if ((size_t)len < rlen && !memcmp(p, ZTE_VAR_CEREG, len))
		return true;
	if ((size_t)len < ilen && !memcmp(p, ZTE_VAR_IPSYS, len))
		return true;

	if ((size_t)len >= alen && !memcmp(p, ZTE_VAR_IPDPADDR, alen))
		return memchr(p, '\r', len) == NULL;
	if ((size_t)len >= clen && !memcmp(p, ZTE_VAR_IPDPACT, clen))
		return memchr(p, '\r', len) == NULL;
	if ((size_t)len >= rlen && !memcmp(p, ZTE_VAR_CEREG, rlen))
		return memchr(p, '\r', len) == NULL;
	if ((size_t)len >= ilen && !memcmp(p, ZTE_VAR_IPSYS, ilen))
		return memchr(p, '\r', len) == NULL;

	return false;
}

static bool zte_var_complete(const u8 *p, int len)
{
	if (len <= 0 || p[len - 1] != '\r')
		return false;
	return !memcmp(p, ZTE_VAR_IPDPADDR, strlen(ZTE_VAR_IPDPADDR)) ||
	       !memcmp(p, ZTE_VAR_IPDPACT, strlen(ZTE_VAR_IPDPACT)) ||
	       !memcmp(p, ZTE_VAR_CEREG, strlen(ZTE_VAR_CEREG)) ||
	       !memcmp(p, ZTE_VAR_IPSYS, strlen(ZTE_VAR_IPSYS));
}

/* ---------------------------------------------------------------------- */
/* link management (private interface)                                    */

static void zte_poll_work_fn(struct work_struct *work)
{
	char resp[256];
	unsigned long flags;
	struct usb_serial_port *at_port;
	bool alive;

	spin_lock_irqsave(&zte_cmd_lock, flags);
	alive = zte_cmd_port != NULL;
	at_port = zte_at_port;
	spin_unlock_irqrestore(&zte_cmd_lock, flags);
	if (!alive)
		return;

	zte_send_cmd(zte_poll_cmds[zte_poll_idx %
				   ARRAY_SIZE(zte_poll_cmds)],
		     resp, sizeof(resp), 1500);

	/* Feed MM an act-bearing CEREG URC at a slow pace: the firmware never
	 * emits one itself, MM relies on URCs (not polling) for registration
	 * and the registration-check result is not used for the technology. */
	if (at_port && (zte_poll_idx % 7) == 0 &&
	    READ_ONCE(zte_cereg_stat) >= 0) {
		char urc[64];

		snprintf(urc, sizeof(urc), "\r\n+CEREG: %d,%d,,,%d\r\n",
			 READ_ONCE(zte_cereg_n), READ_ONCE(zte_cereg_stat),
			 ZTE_ACT_LTE);
		zte_atfix_inject(at_port, urc);
	}

	zte_poll_idx++;
	queue_delayed_work(zte_wq, &zte_poll_work, msecs_to_jiffies(2000));
}

static void zte_init_work_fn(struct work_struct *work)
{
	static const char * const apn_cmnet[] = {
		"46000", "46002", "46004", "46007", "46008"
	};
	static const char * const apn_3gnet[] = {
		"46001", "46006", "46009"
	};
	static const char * const apn_ctnet[] = {
		"46003", "46005", "46011"
	};
	char resp[512];
	char cmd[96];
	const char *apn = "CMNET";
	int i, j, tries;

	/* wait for the modem to come out of boot, then check liveness */
	for (tries = 0; tries < 30; tries++) {
		msleep(1000);
		if (!zte_send_cmd("AT\r", resp, sizeof(resp), 1500) &&
		    zte_cmd_response_ok(resp))
			break;
	}
	if (tries == 30) {
		pr_warn("zte_atfix: modem did not answer AT\n");
		return;
	}

	if (!zte_send_cmd("AT+CIMI\r", resp, sizeof(resp), 2000)) {
		char imsi[32] = "";
		char *digits = resp;

		while (*digits && (*digits < '0' || *digits > '9'))
			digits++;
		if (sscanf(digits, "%31[0-9]", imsi) == 1 && imsi[0]) {
			for (i = 0; i < ARRAY_SIZE(apn_cmnet); i++)
				if (!strncmp(imsi, apn_cmnet[i],
					     strlen(apn_cmnet[i])))
					apn = "CMNET";
			for (i = 0; i < ARRAY_SIZE(apn_3gnet); i++)
				if (!strncmp(imsi, apn_3gnet[i],
					     strlen(apn_3gnet[i])))
					apn = "3gnet";
			for (i = 0; i < ARRAY_SIZE(apn_ctnet); i++)
				if (!strncmp(imsi, apn_ctnet[i],
					     strlen(apn_ctnet[i])))
					apn = "ctnet";
			pr_info("zte_atfix: IMSI %s -> APN %s\n", imsi, apn);
		}
	}

	snprintf(cmd, sizeof(cmd), "AT+CGDCONT=1,IP,%s,,0,0\r", apn);
	for (j = 0; j < 3; j++) {
		zte_send_cmd("AT+ZEACT=2\r", NULL, 0, 1000);
		zte_send_cmd(cmd, NULL, 0, 1500);
		zte_send_cmd("AT+ZGAAT=0\r", NULL, 0, 1000);
		zte_send_cmd("AT+CFUN=1\r", resp, sizeof(resp), 3000);
		zte_send_cmd("AT+ZSET=EXCEPT_RESET,1\r", NULL, 0, 1000);
		zte_send_cmd("AT+ZEMCI=0\r", NULL, 0, 1500);
		if (zte_cmd_response_ok(resp))
			break;
		msleep(2000);
	}

	pr_info("zte_atfix: link init done (APN %s)\n", apn);
	queue_delayed_work(zte_wq, &zte_poll_work, msecs_to_jiffies(500));
}

/* ---------------------------------------------------------------------- */
/* AT fixups                                                              */

static int zte_atfix_port_probe(struct usb_serial_port *port)
{
	struct zte_atfix_port *st;
	int ifnum = zte_ifnum(port);

	st = kzalloc(sizeof(*st), GFP_KERNEL);
	if (!st)
		return -ENOMEM;

	st->ifnum = ifnum;
	st->quiet = (ifnum == ZTE_IF_MODEM || ifnum == ZTE_IF_LOG);

	usb_disable_autosuspend(port->serial->dev);

	usb_set_serial_port_data(port, st);

	if (ifnum == ZTE_IF_AT) {
		unsigned long flags;

		spin_lock_irqsave(&zte_cmd_lock, flags);
		zte_at_port = port;
		spin_unlock_irqrestore(&zte_cmd_lock, flags);
	}

	if (ifnum == ZTE_IF_MODEM) {
		unsigned long flags;

		spin_lock_irqsave(&zte_cmd_lock, flags);
		zte_cmd_port = port;
		spin_unlock_irqrestore(&zte_cmd_lock, flags);

		zte_priv_urb = usb_alloc_urb(0, GFP_KERNEL);
		zte_priv_buf = kmalloc(ZTE_PRIV_BUF_SIZE, GFP_KERNEL);
		if (zte_priv_urb && zte_priv_buf) {
			usb_fill_bulk_urb(zte_priv_urb, port->serial->dev,
					  usb_rcvbulkpipe(port->serial->dev,
							  port->bulk_in_endpointAddress),
					  zte_priv_buf, ZTE_PRIV_BUF_SIZE,
					  zte_priv_read_cb, port);
			if (usb_submit_urb(zte_priv_urb, GFP_KERNEL)) {
				usb_free_urb(zte_priv_urb);
				zte_priv_urb = NULL;
			}
		}
		queue_work(zte_wq, &zte_init_work);
	}

	return 0;
}

static void zte_atfix_port_remove(struct usb_serial_port *port)
{
	struct zte_atfix_port *st = usb_get_serial_port_data(port);
	unsigned long flags;

	spin_lock_irqsave(&zte_cmd_lock, flags);
	if (zte_cmd_port == port) {
		zte_cmd_port = NULL;
		zte_cmd_pending = false;
	}
	if (zte_at_port == port)
		zte_at_port = NULL;
	spin_unlock_irqrestore(&zte_cmd_lock, flags);

	if (zte_priv_urb) {
		usb_kill_urb(zte_priv_urb);
		usb_free_urb(zte_priv_urb);
		zte_priv_urb = NULL;
	}
	kfree(zte_priv_buf);
	zte_priv_buf = NULL;

	spin_lock_irqsave(&zte_dial_lock, flags);
	if (zte_dial_port == port)
		zte_dial_port = NULL;
	spin_unlock_irqrestore(&zte_dial_lock, flags);

	usb_set_serial_port_data(port, NULL);
	kfree(st);
}

/* inject a string into the tty's read stream (as if it came from the modem) */
static void zte_atfix_inject(struct usb_serial_port *port, const char *str)
{
	struct tty_port *tp = &port->port;
	size_t len = strlen(str);
	unsigned char *chars;
	size_t n;

	n = tty_prepare_flip_string(tp, &chars, len);
	if (n < len) {
		dev_warn(&port->dev, "no room to inject response\n");
		return;
	}
	memcpy(chars, str, len);
	tty_flip_buffer_push(tp);
}

static void zte_atfix_forward(struct tty_struct *tty,
			      struct usb_serial_port *port,
			      const u8 *data, int len)
{
	if (len > 0)
		usb_serial_generic_write(tty, port, data, len);
}

/* emit whatever we held back verbatim and reset the candidate state */
static void zte_atfix_discard(struct tty_struct *tty,
			      struct usb_serial_port *port,
			      struct zte_atfix_port *st)
{
	zte_atfix_forward(tty, port, st->pending, st->pending_len);
	st->pending_len = 0;
	st->in_ws46_set = false;
}

/* is pending a complete pattern still waiting for its CR? */
static bool zte_atfix_awaiting_cr(const u8 *p, int len)
{
	for (int i = 0; i < ARRAY_SIZE(zte_patterns); i++) {
		size_t plen = strlen(zte_patterns[i]);

		if ((size_t)len == plen && !memcmp(p, zte_patterns[i], plen))
			return true;
	}
	return false;
}

/* is pending a complete pattern including CR? */
static bool zte_atfix_complete_cr(const u8 *p, int len)
{
	for (int i = 0; i < ARRAY_SIZE(zte_patterns); i++) {
		size_t plen = strlen(zte_patterns[i]);

		if ((size_t)len == plen + 1 && p[plen] == '\r' &&
		    !memcmp(p, zte_patterns[i], plen))
			return true;
	}
	return false;
}

/* is pending a proper prefix of any pattern? */
static bool zte_atfix_prefix(const u8 *p, int len)
{
	for (int i = 0; i < ARRAY_SIZE(zte_patterns); i++) {
		size_t plen = strlen(zte_patterns[i]);

		if ((size_t)len < plen && !memcmp(p, zte_patterns[i], len))
			return true;
	}
	return false;
}

static int zte_atfix_write(struct tty_struct *tty, struct usb_serial_port *port,
			   const unsigned char *buf, int count)
{
	static const char atz_fixup[]  = "ATE0\r";
	static const char gcap_resp[]  =
		"\r\n+GCAP: +CGSM,+CLTE\r\n\r\nOK\r\n";
	static const char ws46_resp[]  =
		"\r\n+WS46: (28)\r\n\r\nOK\r\n";
	static const char ipsys_resp[] =
		"\r\n%IPSYS: 0,1,0\r\n\r\nOK\r\n";
	static const char ok_resp[]    = "\r\nOK\r\n";
	struct zte_atfix_port *st = usb_get_serial_port_data(port);
	int i;

	if (!st)
		return usb_serial_generic_write(tty, port, buf, count);

	for (i = 0; i < count; i++) {
		u8 c = buf[i];

		if (st->in_ws46_set) {
			if (st->pending_len < ZTE_MAX_PENDING)
				st->pending[st->pending_len++] = c;
			if (c == '\r') {
				if (!st->logged_ws46) {
					dev_info(&port->dev,
						 "answering unsupported AT+WS46 set\n");
					st->logged_ws46 = true;
				}
				zte_atfix_inject(port, ok_resp);
				st->pending_len = 0;
				st->in_ws46_set = false;
			}
			continue;
		}

		if (st->pending_len >= ZTE_MAX_PENDING) {
			zte_atfix_discard(tty, port, st);
			continue;
		}
		st->pending[st->pending_len++] = c;

		/* complete patterns waiting for CR stay pending */
		if (zte_atfix_awaiting_cr(st->pending, st->pending_len))
			continue;

		/* variable-length Icera commands: keep until CR (bounded) */
		if (zte_var_hold(st->pending, st->pending_len))
			continue;

		/* "AT+WS46=" followed by a digit: swallow until CR */
		if (st->pending_len == 9 &&
		    !memcmp(st->pending, "AT+WS46=", 8) &&
		    st->pending[8] >= '0' && st->pending[8] <= '9') {
			st->in_ws46_set = true;
			continue;
		}

		/* complete patterns including CR: act on them */
		if (zte_atfix_complete_cr(st->pending, st->pending_len)) {
			if (!memcmp(st->pending, "ATZ\r", 4)) {
				if (!st->logged_atz) {
					dev_info(&port->dev,
						 "rewriting unsupported ATZ to ATE0\n");
					st->logged_atz = true;
				}
				zte_atfix_forward(tty, port, atz_fixup,
						  sizeof(atz_fixup) - 1);
			} else if (!memcmp(st->pending, "AT+GCAP\r", 8)) {
				if (!st->logged_gcap) {
					dev_info(&port->dev,
						 "answering unsupported AT+GCAP\n");
					st->logged_gcap = true;
				}
				zte_atfix_inject(port, gcap_resp);
			} else if (!memcmp(st->pending, "AT%IPSYS?\r", 10)) {
				if (!st->logged_ipsys) {
					dev_info(&port->dev,
						 "answering unsupported AT%%IPSYS? (Icera spoof)\n");
					st->logged_ipsys = true;
				}
				zte_atfix_inject(port, ipsys_resp);
			} else if (!memcmp(st->pending, "AT%NWSTATE\r", 11)) {
				if (st->ifnum == ZTE_IF_AT)
					zte_answer_nwstate(port);
				else
					zte_atfix_forward(tty, port, st->pending,
							  st->pending_len);
			} else if (!memcmp(st->pending, "AT+CSQ\r", 7)) {
				/* modem answers an out-of-range value that MM
				 * would clamp to 100%; normalize it, but only
				 * on the interface MM owns */
				if (st->ifnum == ZTE_IF_AT &&
				    zte_csq_normalize(READ_ONCE(zte_csq_raw)) >= 0)
					zte_answer_csq(port);
				else
					zte_atfix_forward(tty, port, st->pending,
							  st->pending_len);
			} else if (!memcmp(st->pending, "AT+CEREG?\r", 10)) {
				/* the firmware omits the AcT field, so MM can
				 * never learn the access technology */
				if (st->ifnum == ZTE_IF_AT &&
				    READ_ONCE(zte_cereg_stat) >= 0)
					zte_answer_cereg(port);
				else
					zte_atfix_forward(tty, port, st->pending,
							  st->pending_len);
			} else {
				if (!st->logged_ws46) {
					dev_info(&port->dev,
						 "answering unsupported AT+WS46=? query\n");
					st->logged_ws46 = true;
				}
				zte_atfix_inject(port, ws46_resp);
			}
			st->pending_len = 0;
			continue;
		}

		/* variable-length patterns terminated by CR: act on them */
		if (zte_var_complete(st->pending, st->pending_len)) {
			if (!memcmp(st->pending, ZTE_VAR_IPDPADDR,
				    strlen(ZTE_VAR_IPDPADDR)))
				zte_answer_ipdpaddr(port, st->pending);
			else if (!memcmp(st->pending, ZTE_VAR_IPDPACT,
					 strlen(ZTE_VAR_IPDPACT)))
				zte_answer_ipdpact(port, st->pending);
			else if (!memcmp(st->pending, "AT%IPSYS=?\r", 11))
				zte_answer_ipsys_modes(port);
			else if (!memcmp(st->pending, ZTE_VAR_IPSYS,
					 strlen(ZTE_VAR_IPSYS)))
				zte_atfix_inject(port, ok_resp);
			else
				zte_cache_cereg_cmd(tty, port, st);
			st->pending_len = 0;
			continue;
		}

		/* keep the private interfaces invisible to AT probing */
		if (st->quiet && st->pending_len == 3 &&
		    !memcmp(st->pending, "AT\r", 3)) {
			st->pending_len = 0;
			continue;
		}

		/* still a possible prefix? keep holding */
		if (zte_atfix_prefix(st->pending, st->pending_len))
			continue;

		/* not one of our patterns: emit what we held */
		zte_atfix_discard(tty, port, st);
	}

	return count;
}

static void zte_atfix_process_read_urb(struct urb *urb)
{
	struct usb_serial_port *port;
	struct zte_atfix_port *st;
	unsigned long flags;

	if (urb && urb->context) {
		port = urb->context;
		st = usb_get_serial_port_data(port);
		if (st && urb->actual_length) {
			bool done = false;

			zte_sniff_rx(urb->transfer_buffer,
				     urb->actual_length);
			zte_augment_cereg_urc(port, st,
					      urb->transfer_buffer,
					      urb->actual_length);

			/* capture responses to our own commands */
			spin_lock_irqsave(&zte_cmd_lock, flags);
			if (zte_cmd_pending && zte_cmd_port == port) {
				int n = min_t(int, urb->actual_length,
					      (int)sizeof(zte_cmd_resp) - 1 -
					      zte_cmd_resp_len);

				if (n > 0) {
					memcpy(zte_cmd_resp + zte_cmd_resp_len,
					       urb->transfer_buffer, n);
					zte_cmd_resp_len += n;
					zte_cmd_resp[zte_cmd_resp_len] = '\0';
				}
				if (strstr(zte_cmd_resp, "\r\nOK\r\n") ||
				    strstr(zte_cmd_resp, "ERROR")) {
					zte_cmd_pending = false;
					done = true;
				}
			}
			spin_unlock_irqrestore(&zte_cmd_lock, flags);

			if (done) {
				complete(&zte_cmd_done);
				return;
			}
			if (st->quiet && !port->port.tty)
				return;
		}
	}

	usb_serial_generic_process_read_urb(urb);
}

static void zte_atfix_close(struct usb_serial_port *port)
{
	struct tty_struct *tty = port->port.tty;
	struct zte_atfix_port *st = usb_get_serial_port_data(port);

	if (tty && st && st->pending_len)
		zte_atfix_discard(tty, port, st);

	usb_serial_generic_close(port);
}

static struct usb_serial_driver zte_atfix_device = {
	.driver = {
		.owner = THIS_MODULE,
		.name  = "zte_atfix",
	},
	.description     = "ZTE ZX297510 AT fixup and link management",
	.id_table        = zte_atfix_ids,
	.num_ports       = 1,
	.port_probe      = zte_atfix_port_probe,
	.port_remove     = zte_atfix_port_remove,
	.open            = usb_serial_generic_open,
	.close           = zte_atfix_close,
	.write           = zte_atfix_write,
	.process_read_urb = zte_atfix_process_read_urb,
	.chars_in_buffer = usb_serial_generic_chars_in_buffer,
	.throttle        = usb_serial_generic_throttle,
	.unthrottle      = usb_serial_generic_unthrottle,
};

/* ---------------------------------------------------------------------- */
/* device claiming                                                        */
/*
 * The modem's interfaces are also claimed by the generic drivers (option
 * matches all vendor interfaces, qmi_wwan matches the data interface and
 * would crash this firmware with QMI probing), and whichever driver probes
 * first wins.  That races badly on cold boot and on hot plug, so the device
 * must be taken over deterministically:
 *
 *  - on module load, release any competing driver that already grabbed an
 *    interface; our driver registration then binds it right away,
 *  - on hot plug, a USB notifier releases the competitor and force-attaches
 *    our driver (device_driver_attach probes that specific driver, so the
 *    generic drivers never get a chance to keep it).
 *
 * USB devices do not support the driver core's driver_override mechanism,
 * hence the explicit detach/attach dance.
 *
 * interface 0/2/3 -> zte_atfix, interface 1 (ECM) -> zte_ecm (owned by that
 * module, which runs the same logic for its interface).
 */

#define ZTE_ATFIX_NAME	"zte_atfix"

/* not declared in the installed headers */
extern int device_driver_attach(const struct device_driver *drv,
				struct device *dev);

static struct work_struct zte_claim_work;
static struct delayed_work zte_claim_watch_work;
static bool zte_claim_attach;

static void zte_claim_interface(struct usb_interface *intf)
{
	struct device *dev = &intf->dev;
	int ifnum;

	if (!intf->cur_altsetting)
		return;
	ifnum = intf->cur_altsetting->desc.bInterfaceNumber;
	if (ifnum != ZTE_IF_AT && ifnum != ZTE_IF_MODEM &&
	    ifnum != ZTE_IF_LOG)
		return;

	if (dev->driver && strcmp(dev->driver->name, ZTE_ATFIX_NAME)) {
		dev_info(dev, "zte_atfix: releasing competing driver '%s'\n",
			 dev->driver->name);
		device_release_driver(dev);
	}

	/* force-bind our driver when it is already registered */
	if (zte_claim_attach && !dev->driver &&
	    zte_atfix_device.usb_driver)
		device_driver_attach(&zte_atfix_device.usb_driver->driver, dev);
}

static int zte_claim_walk(struct usb_device *udev, void *data)
{
	int i;

	if (!udev->actconfig)
		return 0;
	if (le16_to_cpu(udev->descriptor.idVendor) != ZTE_VID ||
	    le16_to_cpu(udev->descriptor.idProduct) != ZTE_PID)
		return 0;

	for (i = 0; i < udev->actconfig->desc.bNumInterfaces; i++) {
		if (udev->actconfig->interface[i])
			zte_claim_interface(udev->actconfig->interface[i]);
	}
	return 0;
}

static void zte_claim_work_fn(struct work_struct *work)
{
	zte_claim_attach = true;
	usb_for_each_dev(NULL, zte_claim_walk);
	zte_claim_attach = false;
}

/*
 * Safety net: interface reconfiguration (e.g. toggling "authorized") does
 * not generate USB_DEVICE_ADD events, so the generic drivers can grab the
 * interfaces again without any notification.  Re-check at a slow pace and
 * take them back whenever that happened.
 */
static void zte_claim_watch_fn(struct work_struct *work)
{
	zte_claim_work_fn(work);
	queue_delayed_work(system_wq, &zte_claim_watch_work,
			   msecs_to_jiffies(1000));
}

static int zte_usb_notify(struct notifier_block *nb, unsigned long action,
			  void *data)
{
	struct usb_device *udev = data;

	if (action != USB_DEVICE_ADD)
		return NOTIFY_OK;
	if (le16_to_cpu(udev->descriptor.idVendor) != ZTE_VID ||
	    le16_to_cpu(udev->descriptor.idProduct) != ZTE_PID)
		return NOTIFY_OK;

	/* generic drivers may have bound the interfaces during enumeration:
	 * take them back */
	schedule_work(&zte_claim_work);
	return NOTIFY_OK;
}

static struct notifier_block zte_usb_nb = {
	.notifier_call = zte_usb_notify,
};

static struct usb_serial_driver *const zte_atfix_drivers[] = {
	&zte_atfix_device, NULL
};

static int __init zte_atfix_init(void)
{
	int ret;

	spin_lock_init(&zte_cache.lock);
	spin_lock_init(&zte_cmd_lock);
	spin_lock_init(&zte_dial_lock);
	init_completion(&zte_cmd_done);
	INIT_WORK(&zte_init_work, zte_init_work_fn);
	INIT_WORK(&zte_dial_work, zte_dial_work_fn);
	INIT_WORK(&zte_claim_work, zte_claim_work_fn);
	INIT_DELAYED_WORK(&zte_claim_watch_work, zte_claim_watch_fn);
	INIT_DELAYED_WORK(&zte_poll_work, zte_poll_work_fn);

	zte_cmd_buf = kmalloc(256, GFP_KERNEL);
	if (!zte_cmd_buf)
		return -ENOMEM;

	zte_wq = alloc_workqueue("zte_atfix", WQ_UNBOUND, 1);
	if (!zte_wq) {
		kfree(zte_cmd_buf);
		return -ENOMEM;
	}

	/* claim the device before registering, then follow hot plug events */
	usb_for_each_dev(NULL, zte_claim_walk);
	usb_register_notify(&zte_usb_nb);

	ret = usb_serial_register_drivers(zte_atfix_drivers, KBUILD_MODNAME,
					   zte_atfix_ids);
	if (ret) {
		usb_unregister_notify(&zte_usb_nb);
		destroy_workqueue(zte_wq);
		kfree(zte_cmd_buf);
		return ret;
	}

	queue_delayed_work(system_wq, &zte_claim_watch_work,
			   msecs_to_jiffies(1000));
	return 0;
}

static void __exit zte_atfix_exit(void)
{
	usb_unregister_notify(&zte_usb_nb);
	cancel_work_sync(&zte_claim_work);
	cancel_delayed_work_sync(&zte_claim_watch_work);
	usb_serial_deregister_drivers(zte_atfix_drivers);
	cancel_work_sync(&zte_init_work);
	cancel_work_sync(&zte_dial_work);
	cancel_delayed_work_sync(&zte_poll_work);
	destroy_workqueue(zte_wq);
	kfree(zte_cmd_buf);
}

module_init(zte_atfix_init);
module_exit(zte_atfix_exit);

MODULE_DESCRIPTION("ZTE ZX297510 AT fixup and link management");
MODULE_LICENSE("GPL");
