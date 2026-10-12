#ifndef CHUART_BLE_H
#define CHUART_BLE_H

#include <stddef.h>
#include <stdint.h>

#include "ring.h"
#include "tty_params.h"

/*
 * Thin, defensive wrapper around CH9140lib.
 *
 * CH9140lib runs its own GLib main loop on an internal thread, so every
 * callback below is invoked from that thread - never from the caller's.  This
 * module therefore only shares state through a mutex and the ring buffers.
 */

/* A device seen by chuart scan. */
struct ble_device {
	char addr[18];
	char name[64];
	int8_t rssi;
	uint8_t chip_ver;
};

/* Scan for up to `seconds` seconds.  Returns 0 on success, negative errno
 * style values on failure (see ble_adapter_check()). */
int ble_scan(int seconds, void (*on_device)(const struct ble_device *, void *),
	     void *user);

/* Report the local adapter state: 0 when usable, -ENODEV when no adapter or
 * the query timed out, -EPERM when bluetooth is switched off. */
int ble_adapter_check(int *version_out, const char **version_name_out);

const char *ble_version_name(int version);

/* ------------------------------------------------------------------ link --- */

struct ble_link;

struct ble_link_opts {
	const char *notify_uuid; /* characteristic to subscribe to */
	int connect_timeout_ms;
	int pre_scan_seconds;      /* scan once and retry when BlueZ does not
				    * know the device yet; 0 disables that */
	int disconnect_quiesce_ms; /* grace period for the link-down callback */
	ring_t *rx;                /* module -> client */
	void (*rx_hook)(void *user); /* called after data lands in rx */
	void (*link_lost)(void *user, const char *reason);
	void *user;
};

/* Connect and wait for the link to come up.  Returns NULL on failure. */
struct ble_link *ble_link_open(const char *mac, const struct ble_link_opts *opts);

/* Subscribe to notifications, query the MTU and configure the module's UART.
 * Must be called after ble_link_open() succeeded. */
int ble_link_startup(struct ble_link *l, int baud, int databits, int stopbits,
		     int parity, int flow, int dtr, int rts);

uint16_t ble_link_mtu(const struct ble_link *l);

/* The underlying CH9140lib handle, for discovery helpers (chuart features).
 * Declared with the library's tag so ble.h stays free of CH9140lib.h. */
struct _gatt_connection_t *ble_link_handle(struct ble_link *l);

/* Push a parameter set into the module (UART format + modem lines).  Used both
 * at start-up and whenever the client changes the port settings through the
 * CUSE device, so that `picocom -b 9600` really reconfigures the module.
 * Returns 0, or a negative errno when the module refuses. */
int ble_link_apply_params(struct ble_link *l, const struct tty_params *p);

/* Send client data to the module, fragmenting for the negotiated MTU.  Returns
 * bytes accepted (which is always len unless the link died), -1 on error. */
ssize_t ble_link_write(struct ble_link *l, const void *buf, size_t len);

/* Latest modem lines reported by the module (CH9140Ble_Get_Modem_state). */
void ble_link_modem_state(struct ble_link *l, int *dcd, int *ri, int *dsr,
			  int *cts);

int ble_link_is_up(struct ble_link *l);

/* Disconnect and release.  Safe to call on a partially initialised link. */
void ble_link_close(struct ble_link *l);

#endif /* CHUART_BLE_H */
