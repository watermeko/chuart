#define _GNU_SOURCE
#include "ble.h"

#include "log.h"

#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "ch9140lib.h"

#define WRITE_CHUNK_MAX 4096

/* -------------------------------------------------------- adapter / scan --- */

const char *ble_version_name(int version)
{
	/* 0..11 as documented by the vendor; the last entries extend the table
	 * with newer HCI version codes that the library still reports. */
	static const char *names[] = { "V1.0", "V1.1", "V1.2", "V2.0", "V2.1",
				       "V3.0", "V4.0", "V4.1", "V4.2", "V5.0",
				       "V5.1", "V5.2", "V5.3", "V5.4", "V6.0",
				       "V6.1" };

	if (version < 0 || (size_t)version >= sizeof(names) / sizeof(names[0]))
		return "unknown";
	return names[version];
}

int ble_adapter_check(int *version_out, const char **version_name_out)
{
	int version = CH9140BleGetBluetoothVer();

	if (version_out)
		*version_out = version;
	if (version_name_out)
		*version_name_out = ble_version_name(version);

	if (version >= 0)
		return 0;
	if (version == -2)
		return -EPERM; /* adapter exists but is powered off */
	return -ENODEV;        /* query timed out: no usable adapter */
}

/* CH9140lib's scan callback carries no user pointer, so the current scan is
 * held in a module-level slot (one scan at a time, which is all the CLI does). */
static pthread_mutex_t g_scan_lock = PTHREAD_MUTEX_INITIALIZER;
static void (*g_scan_cb)(const struct ble_device *, void *);
static void *g_scan_user;
static int g_scan_running;

static void scan_trampoline(const char *addr, const char *name, int8_t rssi,
			    uint8_t *chip_ver)
{
	struct ble_device dev;
	void (*cb)(const struct ble_device *, void *);
	void *user;

	memset(&dev, 0, sizeof(dev));
	snprintf(dev.addr, sizeof(dev.addr), "%s", addr ? addr : "");
	snprintf(dev.name, sizeof(dev.name), "%s", name ? name : "");
	dev.rssi = rssi;
	/* The library hands over a bare pointer with no documented length, so
	 * only the first byte is read. */
	dev.chip_ver = chip_ver ? chip_ver[0] : 0;

	pthread_mutex_lock(&g_scan_lock);
	cb = g_scan_cb;
	user = g_scan_user;
	pthread_mutex_unlock(&g_scan_lock);

	if (cb)
		cb(&dev, user);
}

int ble_scan(int seconds, void (*on_device)(const struct ble_device *, void *),
	     void *user)
{
	int rc;

	if (seconds <= 0)
		seconds = 5;

	pthread_mutex_lock(&g_scan_lock);
	if (g_scan_running) {
		pthread_mutex_unlock(&g_scan_lock);
		return -EBUSY;
	}
	g_scan_cb = on_device;
	g_scan_user = user;
	g_scan_running = 1;
	pthread_mutex_unlock(&g_scan_lock);

	log_info("scanning for %d second(s)...", seconds);
	rc = CH9140Ble_BLE_Scan(seconds, scan_trampoline);

	pthread_mutex_lock(&g_scan_lock);
	g_scan_cb = NULL;
	g_scan_user = NULL;
	g_scan_running = 0;
	pthread_mutex_unlock(&g_scan_lock);

	if (rc == 1) {
		log_error("bluetooth is not powered on");
		return -EPERM;
	}
	if (rc != 0) {
		log_error("scan failed (CH9140Ble_BLE_Scan returned %d)", rc);
		return -EIO;
	}
	return 0;
}

/* ------------------------------------------------------------------- link --- */

struct ble_link {
	CH9140HANDLE *handle;
	struct ble_link_opts opts;

	pthread_mutex_t lock;
	pthread_cond_t cond;
	int connect_state; /* -1 pending, 0 failed, 1 connected */
	int up;
	int closing;
	uint16_t mtu;
	int dcd, ri, dsr, cts;
	unsigned long rx_dropped;
};

/* Only one link is supported (a BLE serial module accepts a single
 * connection); every CH9140lib callback is routed through this slot. */
static pthread_mutex_t g_link_lock = PTHREAD_MUTEX_INITIALIZER;
static struct ble_link *g_link;

static struct ble_link *link_get(void)
{
	struct ble_link *l;

	pthread_mutex_lock(&g_link_lock);
	l = g_link;
	pthread_mutex_unlock(&g_link_lock);
	return l;
}

static void on_connection_state(CH9140HANDLE *handle, int state)
{
	struct ble_link *l = link_get();

	(void)handle;
	if (!l)
		return;
	pthread_mutex_lock(&l->lock);
	l->connect_state = state ? 1 : 0;
	l->up = state ? 1 : 0;
	pthread_cond_broadcast(&l->cond);
	pthread_mutex_unlock(&l->lock);
	log_debug("ble: connection state -> %s", state ? "connected" : "failed");
}

static void on_disconnect(void *user_data)
{
	struct ble_link *l = link_get();

	(void)user_data;
	if (!l)
		return;
	pthread_mutex_lock(&l->lock);
	if (!l->up) {
		pthread_mutex_unlock(&l->lock);
		return;
	}
	l->up = 0;
	pthread_mutex_unlock(&l->lock);

	log_warn("ble: module disconnected");
	if (l->opts.link_lost)
		l->opts.link_lost(l->opts.user, "module disconnected");
}

static void on_modem_state(bool dcd, bool ri, bool dsr, bool cts)
{
	struct ble_link *l = link_get();

	if (!l)
		return;
	pthread_mutex_lock(&l->lock);
	if (l->dcd != dcd || l->ri != ri || l->dsr != dsr || l->cts != cts) {
		l->dcd = dcd;
		l->ri = ri;
		l->dsr = dsr;
		l->cts = cts;
		pthread_mutex_unlock(&l->lock);
		log_info("ble: modem lines DCD=%d RI=%d DSR=%d CTS=%d", dcd, ri,
			 dsr, cts);
		return;
	}
	l->dcd = dcd;
	l->ri = ri;
	l->dsr = dsr;
	l->cts = cts;
	pthread_mutex_unlock(&l->lock);
}

static void on_notification(const uuid_t *uuid, const uint8_t *data,
			    size_t data_length)
{
	struct ble_link *l = link_get();
	size_t stored;

	(void)uuid;
	if (!l || !data || data_length == 0)
		return;

	/* MODEM status frames start with 0x88; the library checks that byte
	 * itself, so passing ordinary payloads through is harmless.  The frame
	 * is still forwarded to the client for transparency. */
	if (data[0] == 0x88)
		CH9140Ble_Get_Modem_state(data, on_modem_state);

	if (!l->opts.rx)
		return;
	stored = ring_write(l->opts.rx, data, data_length, 0);
	if (stored < data_length) {
		pthread_mutex_lock(&l->lock);
		l->rx_dropped += data_length - stored;
		pthread_mutex_unlock(&l->lock);
		log_warn("ble: rx buffer full, dropped %zu byte(s)",
			 data_length - stored);
	}
	if (l->opts.rx_hook)
		l->opts.rx_hook(l->opts.user);
}

static void deadline_in(struct timespec *ts, int ms)
{
	struct timeval now;

	gettimeofday(&now, NULL);
	ts->tv_sec = now.tv_sec + ms / 1000;
	ts->tv_nsec = (long)now.tv_usec * 1000 + (long)(ms % 1000) * 1000000L;
	if (ts->tv_nsec >= 1000000000L) {
		ts->tv_sec++;
		ts->tv_nsec -= 1000000000L;
	}
}

/* One connect attempt.  Returns 0 when the link came up. */
static int ble_link_try_connect(struct ble_link *l, const char *mac)
{
	int timeout = l->opts.connect_timeout_ms > 0 ? l->opts.connect_timeout_ms
						     : 15000;

	log_info("connecting to %s...", mac);
	pthread_mutex_lock(&l->lock);
	l->connect_state = -1;
	l->up = 0;
	pthread_mutex_unlock(&l->lock);

	l->handle = CH9140Ble_Connect(mac, on_connection_state);
	if (!l->handle) {
		log_error("ble: CH9140Ble_Connect(%s) failed", mac);
		return -EIO;
	}
	CH9140Ble_register_on_disconnect(l->handle, on_disconnect);

	pthread_mutex_lock(&l->lock);
	while (l->connect_state == -1) {
		struct timespec ts;

		deadline_in(&ts, timeout);
		if (pthread_cond_timedwait(&l->cond, &l->lock, &ts) ==
		    ETIMEDOUT) {
			log_error("ble: connection to %s timed out after %d ms", mac,
				  timeout);
			l->connect_state = 0;
			break;
		}
	}
	int connected = l->connect_state == 1;
	l->up = connected ? 1 : 0;
	pthread_mutex_unlock(&l->lock);

	if (connected)
		return 0;

	if (l->handle) {
		CH9140Ble_Disconnect(l->handle);
		l->handle = NULL;
	}
	return -ECONNREFUSED;
}

struct ble_link *ble_link_open(const char *mac, const struct ble_link_opts *opts)
{
	struct ble_link *l;

	if (!mac || !*mac || !opts)
		return NULL;

	l = calloc(1, sizeof(*l));
	if (!l)
		return NULL;
	l->opts = *opts;
	l->connect_state = -1;
	pthread_mutex_init(&l->lock, NULL);
	pthread_cond_init(&l->cond, NULL);

	pthread_mutex_lock(&g_link_lock);
	if (g_link) {
		pthread_mutex_unlock(&g_link_lock);
		log_error("ble: another link is already open");
		pthread_mutex_destroy(&l->lock);
		pthread_cond_destroy(&l->cond);
		free(l);
		return NULL;
	}
	g_link = l;
	pthread_mutex_unlock(&g_link_lock);

	for (int attempt = 0; attempt < 2; attempt++) {
		if (ble_link_try_connect(l, mac) == 0)
			goto connected;
		if (attempt == 0 && l->opts.pre_scan_seconds > 0) {
			/* CH9140Ble_Connect() needs the device to be known to
			 * BlueZ; a scan is also the first step of the vendor's
			 * documented call sequence, so fall back to it once. */
			log_warn("ble: %s is not known to BlueZ yet, scanning for "
				 "%d second(s) and retrying", mac,
				 l->opts.pre_scan_seconds);
			ble_scan(l->opts.pre_scan_seconds, NULL, NULL);
			continue;
		}
		break;
	}

	log_error("ble: could not connect to %s", mac);
	ble_link_close(l);
	return NULL;

connected:
	log_info("ble: connected to %s", mac);
	return l;
}

int ble_link_startup(struct ble_link *l, int baud, int databits, int stopbits,
		     int parity, int flow, int dtr, int rts)
{
	uint16_t mtu = 0;
	int rc;

	if (!l || !l->handle)
		return -EINVAL;

	CH9140Ble_register_notification(l->handle, on_notification);

	rc = CH9140Ble_Get_MTU(l->handle, &mtu);
	if (rc == 0) {
		l->mtu = mtu;
		log_info("ble: MTU %u", mtu);
	} else {
		log_warn("ble: MTU query failed (%d), assuming 23", rc);
		l->mtu = 23;
	}

	rc = CH9140Ble_Open_Notification(l->handle, l->opts.notify_uuid);
	if (rc != 0) {
		log_error("ble: cannot subscribe to %s (rc=%d)", l->opts.notify_uuid,
			  rc);
		return -EIO;
	}
	log_info("ble: notifications enabled on %s", l->opts.notify_uuid);

	rc = CH9140Ble_Set_SerialBaud(l->handle, baud, databits, stopbits, parity);
	if (rc != 0) {
		static const char parity_char[] = { 'N', 'O', 'E', 'M', 'S' };

		log_error("ble: cannot set the module UART to %d %d%c%d (rc=%d)",
			  baud, databits,
			  parity >= 0 && parity <= 4 ? parity_char[parity] : '?',
			  stopbits, rc);
		return -EIO;
	}
	log_info("ble: module UART set to %d %d%s%d", baud, databits,
		 parity == 1 ? "O" : parity == 2 ? "E" : "N", stopbits);

	rc = CH9140Ble_Set_Modem(l->handle, flow, dtr, rts);
	if (rc != 0) {
		log_error("ble: cannot apply modem settings (flow=%d dtr=%d rts=%d)",
			  flow, dtr, rts);
		return -EIO;
	}
	log_info("ble: flow control %s, DTR=%d RTS=%d", flow ? "on" : "off", dtr,
		 rts);
	return 0;
}

uint16_t ble_link_mtu(const struct ble_link *l)
{
	return l ? l->mtu : 0;
}

struct _gatt_connection_t *ble_link_handle(struct ble_link *l)
{
	return l ? l->handle : NULL;
}

int ble_link_is_up(struct ble_link *l)
{
	int up;

	if (!l)
		return 0;
	pthread_mutex_lock(&l->lock);
	up = l->up;
	pthread_mutex_unlock(&l->lock);
	return up;
}

void ble_link_modem_state(struct ble_link *l, int *dcd, int *ri, int *dsr,
			  int *cts)
{
	if (!l)
		return;
	pthread_mutex_lock(&l->lock);
	if (dcd)
		*dcd = l->dcd;
	if (ri)
		*ri = l->ri;
	if (dsr)
		*dsr = l->dsr;
	if (cts)
		*cts = l->cts;
	pthread_mutex_unlock(&l->lock);
}

ssize_t ble_link_write(struct ble_link *l, const void *buf, size_t len)
{
	const char *p = buf;
	size_t done = 0;

	if (!l || !l->handle)
		return -1;

	while (done < len) {
		size_t chunk = len - done;

		if (chunk > WRITE_CHUNK_MAX)
			chunk = WRITE_CHUNK_MAX;
		/* CH9140BleUart_Write_Buffer() fragments internally using the
		 * MTU and rejects anything above 4096 bytes. */
		int rc = CH9140BleUart_Write_Buffer(l->handle, (char *)(p + done),
						    (int)chunk, l->mtu);

		if (rc <= 0) {
			log_error("ble: write failed (rc=%d after %zu byte(s))", rc,
				  done);
			return -1;
		}
		done += (size_t)rc;
		if ((size_t)rc < chunk)
			break;
	}
	return (ssize_t)done;
}

void ble_link_close(struct ble_link *l)
{
	if (!l)
		return;

	pthread_mutex_lock(&g_link_lock);
	if (g_link == l)
		g_link = NULL;
	pthread_mutex_unlock(&g_link_lock);

	pthread_mutex_lock(&l->lock);
	l->closing = 1;
	l->up = 0;
	pthread_mutex_unlock(&l->lock);

	if (l->handle) {
		CH9140Ble_Disconnect(l->handle);
		l->handle = NULL;
	}

	/* CH9140lib dispatches callbacks from its own thread; give any in-flight
	 * callback a moment to notice that the link slot is gone before the
	 * memory is released. */
	if (l->opts.disconnect_quiesce_ms > 0)
		usleep((useconds_t)l->opts.disconnect_quiesce_ms * 1000);

	if (l->rx_dropped)
		log_warn("ble: %lu byte(s) were dropped in total", l->rx_dropped);

	pthread_mutex_destroy(&l->lock);
	pthread_cond_destroy(&l->cond);
	free(l);
}
