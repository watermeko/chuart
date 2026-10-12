#ifndef CHUART_CUSE_TTY_H
#define CHUART_CUSE_TTY_H

#include <sys/types.h>

#include "ring.h"
#include "tty_params.h"

/*
 * A /dev/ttyXXX character device backed by CUSE (CUSE = character device in
 * userspace, i.e. the CUSE part of FUSE).
 *
 * The device emulates just enough of a tty for serial terminal programs such
 * as picocom and minicom:
 *
 *   - TCGETS/TCSETS(2) with the *kernel* struct termios layout, which is what
 *     glibc passes down.  This is also what makes isatty(3) return true, and
 *     picocom refuses to run when it does not;
 *   - TIOCMGET/TIOCMBIS/TIOCMBIC/TIOCMSET, TCFLSH, TCSBRK, TIOCGWINSZ,
 *     FIONREAD and a few no-op ioctls.
 *
 * Data does not flow through this module directly: the BLE layer pushes
 * received bytes into rx_ring and calls cuse_tty_rx_ready(); client writes are
 * stored into tx_ring, which the BLE writer thread drains.
 */

struct cuse_tty_opts {
	const char *name;      /* device name below /dev, e.g. "ttyCH9140" */
	const char *owner;     /* optional "user[:group]" to chown the node to */
	mode_t mode;           /* node permissions, 0 = keep default */
	int baud;              /* initial values, also reported by TCGETS */
	int databits;
	int stopbits;
	int parity;            /* 0 none, 1 odd, 2 even, 3 mark, 4 space */
	int flow;              /* hardware flow control reported via CRTSCTS */
	int dtr;               /* initial modem line state */
	int rts;
	size_t rx_capacity;
	size_t tx_capacity;

	/* Called from a FUSE worker thread when the client changes the port
	 * parameters (tcsetattr, TIOCMBIS/BIC/SET).  Returns 0 when the change was
	 * applied (TCGETS then reports the new values), 1 when it was accepted but
	 * deliberately not applied (TCGETS keeps reporting the real port state so
	 * the client can notice the mismatch), or a negative errno to fail the
	 * ioctl.  NULL means client changes are only stored locally. */
	int (*apply_params)(void *user, const struct tty_params *params);
	void *apply_params_user;
};

typedef struct cuse_tty cuse_tty;

/* Create the device node and start the FUSE request loop.
 * Returns 0 on success; on failure the node is not created. */
int cuse_tty_start(const struct cuse_tty_opts *opts, cuse_tty **out);

/* "/dev/<name>" as it should be shown to the user. */
const char *cuse_tty_path(const cuse_tty *t);

/* Ring the BLE layer reads from to obtain data destined for the client. */
ring_t *cuse_tty_rx_ring(cuse_tty *t);

/* Ring the BLE layer drains to send client data to the module. */
ring_t *cuse_tty_tx_ring(cuse_tty *t);

/* Called by the BLE layer (any thread) after pushing bytes into rx_ring. */
void cuse_tty_rx_ready(cuse_tty *t);

/* Called by the BLE layer (any thread) after draining tx_ring, so that pollers
 * waiting for writability are woken up. */
void cuse_tty_tx_space(cuse_tty *t);

/* The BLE link is gone: fail pending reads, wake pollers, reject new writes. */
void cuse_tty_link_lost(cuse_tty *t, const char *reason);

/* Tear down the session.  Blocks until the FUSE thread has exited. */
void cuse_tty_stop(cuse_tty *t);

#endif /* CHUART_CUSE_TTY_H */
