#define _GNU_SOURCE
#define FUSE_USE_VERSION 31

#include "cuse_tty.h"

#include "log.h"

#include <cuse_lowlevel.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <fuse_lowlevel.h>
#include <grp.h>
#include <poll.h>
#include <pthread.h>
#include <pwd.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <termios.h>
#include <unistd.h>

/*
 * glibc's <termios.h> describes the *user* struct termios (NCCS = 32, 60
 * bytes), but the TCGETS ioctl deals in the kernel layout (NCCS = 19, 36
 * bytes, no c_ispeed/c_ospeed).  libc translates between the two, so a CUSE
 * device must traffic in the kernel layout - handing back 60 bytes would
 * overflow the caller's stack buffer.  See include/uapi/asm-generic/termbits.h.
 */
#define KT_NCCS 19

struct kern_termios {
	uint32_t c_iflag;
	uint32_t c_oflag;
	uint32_t c_cflag;
	uint32_t c_lflag;
	uint8_t c_line;
	uint8_t c_cc[KT_NCCS];
};

struct kern_termios2 {
	struct kern_termios t;
	uint32_t c_ispeed;
	uint32_t c_ospeed;
};

/* Not exposed by glibc's <termios.h>. */
#define TCGETS2_NR 0x802c542a
#define TCSETS2_NR 0x402c542b
#define TCSETSW2_NR 0x402c542c
#define TCSETSF2_NR 0x402c542d

#define MAX_PENDING_READS 64
#define MAX_POLL_HANDLES 8
#define READ_CHUNK 4096

struct pending_read {
	fuse_req_t req;
	size_t size;
};

struct cuse_tty {
	char name[64];
	char path[80];
	struct cuse_tty_opts opts;

	ring_t *rx;
	ring_t *tx;

	pthread_mutex_t lock;
	int link_up;
	int stopping;
	int open_count;

	struct kern_termios2 tio;
	struct tty_params params;
	int params_known; /* has a client ever told us the port parameters? */

	struct pending_read reads[MAX_PENDING_READS];
	size_t nreads;
	struct fuse_pollhandle *polls[MAX_POLL_HANDLES];
	size_t npolls;

	struct fuse_session *se;
	pthread_t thread;
	int thread_started;
};

/* ------------------------------------------------------------------ termios */

/*
 * This program is a pure mapping between the device node and the module: it
 * does not configure the module's UART on its own.  Until a client sets the port
 * parameters, their real values are unknown, so TCGETS reports this placeholder
 * and the *first* client request is always forwarded (even if it happens to
 * match the placeholder, which would otherwise be skipped as a no-op).
 */
#define PLACEHOLDER_BAUD 115200

/* Baud rate as encoded in c_cflag & CBAUD.  Rates without a Bxxx code only
 * survive through termios2 (c_ispeed/c_ospeed), which is what picocom uses for
 * custom baud rates. */
static uint32_t baud_to_cflag(int baud)
{
	switch (baud) {
	case 0: return B0;
	case 50: return B50;
	case 75: return B75;
	case 110: return B110;
	case 134: return B134;
	case 150: return B150;
	case 200: return B200;
	case 300: return B300;
	case 600: return B600;
	case 1200: return B1200;
	case 1800: return B1800;
	case 2400: return B2400;
	case 4800: return B4800;
	case 9600: return B9600;
	case 19200: return B19200;
	case 38400: return B38400;
	case 57600: return B57600;
	case 115200: return B115200;
	case 230400: return B230400;
	case 460800: return B460800;
	case 500000: return B500000;
	case 576000: return B576000;
	case 921600: return B921600;
	case 1000000: return B1000000;
	case 1152000: return B1152000;
	case 1500000: return B1500000;
	case 2000000: return B2000000;
	case 2500000: return B2500000;
	case 3000000: return B3000000;
	case 3500000: return B3500000;
	case 4000000: return B4000000;
	default: return B38400;
	}
}

/* Reverse direction: which rate does this CBAUD field encode?  Zero means
 * "no idea" (a code we do not have in the table, e.g. a custom BOTHER rate
 * that only survives in termios2's c_ospeed). */
static int cflag_to_baud(uint32_t cflag, uint32_t ospeed)
{
	switch (cflag & (CBAUD | CBAUDEX)) {
	case B0: return 0;
	case B50: return 50;
	case B75: return 75;
	case B110: return 110;
	case B134: return 134;
	case B150: return 150;
	case B200: return 200;
	case B300: return 300;
	case B600: return 600;
	case B1200: return 1200;
	case B1800: return 1800;
	case B2400: return 2400;
	case B4800: return 4800;
	case B9600: return 9600;
	case B19200: return 19200;
	case B38400: return 38400;
	case B57600: return 57600;
	case B115200: return 115200;
	case B230400: return 230400;
	case B460800: return 460800;
	case B500000: return 500000;
	case B576000: return 576000;
	case B921600: return 921600;
	case B1000000: return 1000000;
	case B1152000: return 1152000;
	case B1500000: return 1500000;
	case B2000000: return 2000000;
	case B2500000: return 2500000;
	case B3000000: return 3000000;
	case B3500000: return 3500000;
	case B4000000: return 4000000;
	default:
		/* BOTHER (and friends): termios2 carries the real rate. */
		return ospeed > 0 ? (int)ospeed : 0;
	}
}

/*
 * Decode the parameters a client wants from the termios it just wrote.  Only
 * the fields it can express are taken: baud/databits/stopbits/parity/flow come
 * from c_cflag (plus c_ospeed for custom rates), while DTR/RTS are kept as they
 * are because they belong to TIOCMBIS/TIOCMBIC/TIOCMSET, not to termios.
 */
static void params_from_termios(const struct kern_termios *k, uint32_t ospeed,
				struct tty_params *out)
{
	int baud = cflag_to_baud(k->c_cflag, ospeed);

	if (baud > 0)
		out->baud = baud;

	switch (k->c_cflag & CSIZE) {
	case CS5: out->databits = 5; break;
	case CS6: out->databits = 6; break;
	case CS7: out->databits = 7; break;
	default: out->databits = 8; break;
	}

	out->stopbits = (k->c_cflag & CSTOPB) ? 2 : 1;

	if (!(k->c_cflag & PARENB))
		out->parity = 0;
	else if (k->c_cflag & CMSPAR)
		out->parity = (k->c_cflag & PARODD) ? 3 : 4; /* mark : space */
	else
		out->parity = (k->c_cflag & PARODD) ? 1 : 2;

	out->flow = (k->c_cflag & CRTSCTS) ? 1 : 0;
}

static void tty_build_termios(struct cuse_tty *t)
{
	uint32_t cflag;

	memset(&t->tio, 0, sizeof(t->tio));
	cflag = baud_to_cflag(t->params.baud) | CREAD | CLOCAL | HUPCL;

	switch (t->params.databits) {
	case 5: cflag |= CS5; break;
	case 6: cflag |= CS6; break;
	case 7: cflag |= CS7; break;
	default: cflag |= CS8; break;
	}
	if (t->params.stopbits == 2)
		cflag |= CSTOPB;
	switch (t->params.parity) {
	case 1:
		cflag |= PARENB | PARODD;
		break;
	case 2:
		cflag |= PARENB;
		break;
	case 3:
		cflag |= PARENB | PARODD | CMSPAR;
		break;
	case 4:
		cflag |= PARENB | CMSPAR;
		break;
	default:
		break;
	}
	if (t->params.flow)
		cflag |= CRTSCTS;

	t->tio.t.c_cflag = cflag;
	t->tio.t.c_iflag = ICRNL | IXON;
	t->tio.t.c_oflag = OPOST | ONLCR;
	t->tio.t.c_lflag = 0;
	t->tio.t.c_cc[VMIN] = 1;
	t->tio.t.c_cc[VTIME] = 0;
	t->tio.c_ispeed = (uint32_t)t->params.baud;
	t->tio.c_ospeed = (uint32_t)t->params.baud;
}

/* ------------------------------------------------------------------- ioctl */

/*
 * With CUSE_UNRESTRICTED_IOCTL the kernel cannot size the argument itself
 * (Linux defines TCGETS as 0x5401, with no _IOC_SIZE), so requests arrive with
 * empty buffers.  The daemon answers with fuse_reply_ioctl_retry() to describe
 * the buffer it wants; the request is then re-sent with in_bufsz/out_bufsz
 * filled in and in_buf carrying the caller's data.
 */
static void retry_in(fuse_req_t req, void *arg, size_t size)
{
	struct iovec iov = { arg, size };

	fuse_reply_ioctl_retry(req, &iov, 1, NULL, 0);
}

static void retry_out(fuse_req_t req, void *arg, size_t size)
{
	struct iovec iov = { arg, size };

	fuse_reply_ioctl_retry(req, NULL, 0, &iov, 1);
}

/*
 * Push a client-requested parameter set to the application layer (which
 * forwards it to the module) and, on success, make it the state reported by
 * TCGETS.  Called with t->lock NOT held: applying parameters performs a D-Bus
 * round trip to the module, which must not block the rx path.
 */
/*
 * Forward a client's parameter set to the application layer and, when that
 * succeeds, report the client's termios back verbatim.
 *
 * Echoing the client's own termios matters: terminal programs re-read the port
 * after configuring it (picocom's term_apply does tcsetattr then tcgetattr) and
 * complain about a "settings mismatch" if what comes back differs.  Only the
 * port parameters are forwarded to the module; the rest of the termios (raw
 * flags, control characters) is the client's business and is stored untouched.
 *
 * `raw`/`raw_len` is the termios structure the client wrote (kernel layout).
 * Returns 0 (applied), 1 (accepted but deliberately not applied) or a negative
 * errno to fail the ioctl.
 */
/* A real port reports back exactly what was written to it, so keep the client's
 * termios verbatim; only the forwarded parameters live in t->params. */
static void store_client_termios(struct cuse_tty *t, const void *raw,
				 size_t raw_len)
{
	if (!raw || raw_len < sizeof(t->tio.t))
		return;
	memcpy(&t->tio.t, raw, sizeof(t->tio.t));
	if (raw_len >= sizeof(t->tio))
		memcpy(&t->tio, raw, sizeof(t->tio));
	else
		t->tio.c_ispeed = t->tio.c_ospeed = (uint32_t)t->params.baud;
}

static int commit_params(struct cuse_tty *t, const void *raw, size_t raw_len,
			 struct tty_params *want)
{
	int changed;
	int rc = 0;
	char buf[64];

	pthread_mutex_lock(&t->lock);
	changed = !t->params_known || !tty_params_equal(&t->params, want);
	if (!changed) {
		/* Same port parameters, but the client may still have written
		 * different termios flags: echo them back as a port would. */
		store_client_termios(t, raw, raw_len);
		pthread_mutex_unlock(&t->lock);
		return 0;
	}
	pthread_mutex_unlock(&t->lock);

	tty_params_format(want, buf, sizeof(buf));
	log_info("cuse: client requested %s", buf);

	if (!t->opts.apply_params) {
		pthread_mutex_lock(&t->lock);
		t->params = *want;
		t->params_known = 1;
		store_client_termios(t, raw, raw_len);
		pthread_mutex_unlock(&t->lock);
		return 0;
	}

	if (t->opts.apply_params) {
		struct timeval t0, t1;

		gettimeofday(&t0, NULL);
		rc = t->opts.apply_params(t->opts.apply_params_user, want);
		gettimeofday(&t1, NULL);
		long ms = (t1.tv_sec - t0.tv_sec) * 1000L +
			  (t1.tv_usec - t0.tv_usec) / 1000L;

		if (rc < 0) {
			log_warn("cuse: cannot apply %s: %s", buf, strerror(-rc));
			return rc; /* leave the reported state untouched */
		}
		if (rc > 0)
			return 0; /* accepted, deliberately not applied */
		log_debug("cuse: %s applied in %ld ms", buf, ms);
	}

	pthread_mutex_lock(&t->lock);
	t->params = *want;
	t->params_known = 1;
	store_client_termios(t, raw, raw_len);
	pthread_mutex_unlock(&t->lock);
	return 0;
}

static void op_ioctl(fuse_req_t req, int cmd, void *arg, struct fuse_file_info *fi,
		     unsigned int flags, const void *in_buf, size_t in_bufsz,
		     size_t out_bufsz)
{
	struct cuse_tty *t = fuse_req_userdata(req);
	int ioctl_ret = 0;

	(void)fi;

	if (flags & FUSE_IOCTL_COMPAT) {
		fuse_reply_err(req, ENOSYS);
		return;
	}

	switch (cmd) {
	case TCGETS:
		if (!out_bufsz) {
			retry_out(req, arg, sizeof(struct kern_termios));
			return;
		}
		pthread_mutex_lock(&t->lock);
		fuse_reply_ioctl(req, ioctl_ret, &t->tio.t, sizeof(t->tio.t));
		pthread_mutex_unlock(&t->lock);
		return;

	case TCSETS:
	case TCSETSW:
	case TCSETSF: {
		struct kern_termios k;
		struct tty_params want;
		int rc;

		if (!in_bufsz) {
			retry_in(req, arg, sizeof(struct kern_termios));
			return;
		}
		memcpy(&k, in_buf, sizeof(k));
		pthread_mutex_lock(&t->lock);
		want = t->params; /* DTR/RTS are not part of termios */
		pthread_mutex_unlock(&t->lock);
		params_from_termios(&k, 0, &want);

		rc = commit_params(t, &k, sizeof(k), &want);
		if (rc != 0) {
			fuse_reply_err(req, -rc);
			return;
		}
		fuse_reply_ioctl(req, ioctl_ret, NULL, 0);
		return;
	}

	case TCGETS2_NR:
		if (!out_bufsz) {
			retry_out(req, arg, sizeof(struct kern_termios2));
			return;
		}
		pthread_mutex_lock(&t->lock);
		fuse_reply_ioctl(req, ioctl_ret, &t->tio, sizeof(t->tio));
		pthread_mutex_unlock(&t->lock);
		return;

	case TCSETS2_NR:
	case TCSETSW2_NR:
	case TCSETSF2_NR: {
		struct kern_termios2 k;
		struct tty_params want;
		int rc;

		if (!in_bufsz) {
			retry_in(req, arg, sizeof(struct kern_termios2));
			return;
		}
		memcpy(&k, in_buf, sizeof(k));
		pthread_mutex_lock(&t->lock);
		want = t->params;
		pthread_mutex_unlock(&t->lock);
		/* c_ispeed == 0 means "same as ospeed" (see termios(3)). */
		params_from_termios(&k.t,
				    k.c_ospeed ? k.c_ospeed : k.c_ispeed,
				    &want);

		rc = commit_params(t, &k, sizeof(k), &want);
		if (rc != 0) {
			fuse_reply_err(req, -rc);
			return;
		}
		fuse_reply_ioctl(req, ioctl_ret, NULL, 0);
		return;
	}

	case TIOCMGET: {
		int status = TIOCM_CTS | TIOCM_DSR | TIOCM_CAR;

		if (!out_bufsz) {
			retry_out(req, arg, sizeof(status));
			return;
		}
		fuse_reply_ioctl(req, ioctl_ret, &status, sizeof(status));
		return;
	}

	case TIOCMBIS:
	case TIOCMBIC:
	case TIOCMSET: {
		int bits;
		struct tty_params want;
		int rc;

		if (!in_bufsz) {
			retry_in(req, arg, sizeof(int));
			return;
		}
		memcpy(&bits, in_buf, sizeof(bits));
		pthread_mutex_lock(&t->lock);
		want = t->params;
		pthread_mutex_unlock(&t->lock);

		if (cmd == TIOCMBIS)
			want.dtr |= (bits & TIOCM_DTR) ? 1 : 0,
			    want.rts |= (bits & TIOCM_RTS) ? 1 : 0;
		else if (cmd == TIOCMBIC)
			want.dtr &= (bits & TIOCM_DTR) ? 0 : 1,
			    want.rts &= (bits & TIOCM_RTS) ? 0 : 1;
		else
			want.dtr = (bits & TIOCM_DTR) ? 1 : 0,
			    want.rts = (bits & TIOCM_RTS) ? 1 : 0;

		rc = commit_params(t, NULL, 0, &want);
		if (rc != 0) {
			fuse_reply_err(req, -rc);
			return;
		}
		fuse_reply_ioctl(req, ioctl_ret, NULL, 0);
		return;
	}

	case TIOCGWINSZ: {
		struct winsize ws;

		if (!out_bufsz) {
			retry_out(req, arg, sizeof(ws));
			return;
		}
		memset(&ws, 0, sizeof(ws));
		ws.ws_col = 80;
		ws.ws_row = 24;
		fuse_reply_ioctl(req, ioctl_ret, &ws, sizeof(ws));
		return;
	}

	case TIOCSWINSZ:
		if (!in_bufsz) {
			retry_in(req, arg, sizeof(struct winsize));
			return;
		}
		fuse_reply_ioctl(req, ioctl_ret, NULL, 0);
		return;

	case FIONREAD: {
		int pending;

		if (!out_bufsz) {
			retry_out(req, arg, sizeof(pending));
			return;
		}
		pending = (int)ring_count(t->rx);
		fuse_reply_ioctl(req, ioctl_ret, &pending, sizeof(pending));
		return;
	}

	case TCFLSH:
	case TCXONC:
	case TCSBRK:
	case TCSBRKP:
	case TIOCEXCL:
	case TIOCNXCL:
	case TIOCNOTTY:
		/* Argument is passed by value, so nothing to fetch. */
		fuse_reply_ioctl(req, ioctl_ret, NULL, 0);
		return;

	default:
		log_debug("cuse: unhandled ioctl 0x%x", (unsigned)cmd);
		fuse_reply_err(req, ENOTTY);
		return;
	}
}

/* --------------------------------------------------------------- data path */

/* Waking a poller means dropping the handle: the kernel re-runs the poll
 * handler to learn the new readiness, so a handle is single-use. */
static void notify_pollers_locked(struct cuse_tty *t)
{
	size_t i;

	for (i = 0; i < t->npolls; i++) {
		if (!t->polls[i])
			continue;
		fuse_lowlevel_notify_poll(t->polls[i]);
		fuse_pollhandle_destroy(t->polls[i]);
		t->polls[i] = NULL;
	}
	t->npolls = 0;
}

/* Satisfy deferred reads from the rx ring.  Called with t->lock held; replies
 * are sent under the lock so that cuse_tty_stop() cannot free the session in
 * between. */
static void service_reads_locked(struct cuse_tty *t)
{
	unsigned char buf[READ_CHUNK];

	while (t->nreads > 0) {
		struct pending_read *pr = &t->reads[0];
		size_t want = pr->size < sizeof(buf) ? pr->size : sizeof(buf);
		ssize_t got = ring_read(t->rx, buf, want, 0);

		if (got <= 0)
			return;
		fuse_reply_buf(pr->req, (const char *)buf, (size_t)got);
		memmove(&t->reads[0], &t->reads[1],
			(t->nreads - 1) * sizeof(t->reads[0]));
		t->nreads--;
	}
}

void cuse_tty_rx_ready(cuse_tty *t)
{
	pthread_mutex_lock(&t->lock);
	if (t->stopping) {
		pthread_mutex_unlock(&t->lock);
		return;
	}
	service_reads_locked(t);
	if (t->nreads == 0 && ring_count(t->rx) > 0)
		notify_pollers_locked(t);
	pthread_mutex_unlock(&t->lock);
}

void cuse_tty_tx_space(cuse_tty *t)
{
	if (!t)
		return;
	pthread_mutex_lock(&t->lock);
	if (t->npolls > 0)
		notify_pollers_locked(t);
	pthread_mutex_unlock(&t->lock);
}

void cuse_tty_link_lost(cuse_tty *t, const char *reason)
{
	size_t i;

	if (!t)
		return;
	pthread_mutex_lock(&t->lock);
	if (!t->link_up) {
		pthread_mutex_unlock(&t->lock);
		return;
	}
	t->link_up = 0;
	if (t->nreads)
		log_warn("cuse: link lost (%s), failing %zu pending read(s)",
			 reason, t->nreads);
	for (i = 0; i < t->nreads; i++)
		fuse_reply_err(t->reads[i].req, EIO);
	t->nreads = 0;
	notify_pollers_locked(t);
	pthread_mutex_unlock(&t->lock);

	ring_close(t->rx);
	ring_close(t->tx);
}

/* ---------------------------------------------------------------------- ops */

static void op_open(fuse_req_t req, struct fuse_file_info *fi)
{
	struct cuse_tty *t = fuse_req_userdata(req);

	pthread_mutex_lock(&t->lock);
	t->open_count++;
	log_debug("cuse: open (flags=0x%x), %d fd(s) now open", fi->flags,
		  t->open_count);
	fuse_reply_open(req, fi);
	pthread_mutex_unlock(&t->lock);
}

static void op_release(fuse_req_t req, struct fuse_file_info *fi)
{
	struct cuse_tty *t = fuse_req_userdata(req);

	(void)fi;
	pthread_mutex_lock(&t->lock);
	if (t->open_count > 0)
		t->open_count--;
	log_debug("cuse: release, %d fd(s) left open", t->open_count);
	fuse_reply_err(req, 0);
	pthread_mutex_unlock(&t->lock);
}

static void op_read(fuse_req_t req, size_t size, off_t off,
		    struct fuse_file_info *fi)
{
	struct cuse_tty *t = fuse_req_userdata(req);
	unsigned char buf[READ_CHUNK];
	ssize_t got;

	(void)fi;
	(void)off; /* stream device: offsets carry no meaning */

	if (size == 0) {
		fuse_reply_buf(req, NULL, 0);
		return;
	}

	pthread_mutex_lock(&t->lock);
	got = ring_read(t->rx, buf, size < sizeof(buf) ? size : sizeof(buf), 0);
	if (got > 0) {
		fuse_reply_buf(req, (const char *)buf, (size_t)got);
		pthread_mutex_unlock(&t->lock);
		return;
	}
	if (got < 0 || !t->link_up) {
		fuse_reply_err(req, EIO);
		pthread_mutex_unlock(&t->lock);
		return;
	}
	if (t->nreads == MAX_PENDING_READS) {
		log_warn("cuse: %zu reads already queued, replying EAGAIN",
			 t->nreads);
		fuse_reply_err(req, EAGAIN);
		pthread_mutex_unlock(&t->lock);
		return;
	}
	t->reads[t->nreads].req = req;
	t->reads[t->nreads].size = size;
	t->nreads++;
	log_debug("cuse: read of %zu byte(s) deferred (%zu queued)", size,
		  t->nreads);
	pthread_mutex_unlock(&t->lock);
}

static void op_write(fuse_req_t req, const char *buf, size_t size, off_t off,
		     struct fuse_file_info *fi)
{
	struct cuse_tty *t = fuse_req_userdata(req);
	size_t stored;

	(void)fi;
	(void)off;

	if (size == 0) {
		fuse_reply_write(req, 0);
		return;
	}

	/* Block until everything is queued: terminal programs treat a short
	 * write as fatal, and a real tty blocks here too.  Link loss closes the
	 * ring and unblocks the wait. */
	stored = ring_write(t->tx, buf, size, -1);
	if (stored < size) {
		log_warn("cuse: write dropped (%zu/%zu): link is down", stored,
			 size);
		fuse_reply_err(req, EIO);
		return;
	}
	log_debug("cuse: queued %zu byte(s) for the module", size);
	fuse_reply_write(req, size);
}

static void op_poll(fuse_req_t req, struct fuse_file_info *fi,
		    struct fuse_pollhandle *ph)
{
	struct cuse_tty *t = fuse_req_userdata(req);
	unsigned revents = 0;

	(void)fi;

	pthread_mutex_lock(&t->lock);
	if (!t->link_up)
		revents |= POLLERR | POLLHUP;
	if (ring_count(t->rx) > 0)
		revents |= POLLIN | POLLRDNORM;
	if (ring_count(t->tx) < ring_capacity(t->tx))
		revents |= POLLOUT | POLLWRNORM;
	if (ph) {
		if (t->npolls == MAX_POLL_HANDLES)
			notify_pollers_locked(t);
		t->polls[t->npolls++] = ph;
	}
	fuse_reply_poll(req, revents);
	pthread_mutex_unlock(&t->lock);
}

static void op_flush(fuse_req_t req, struct fuse_file_info *fi)
{
	(void)fi;
	fuse_reply_err(req, 0);
}

static void op_fsync(fuse_req_t req, int datasync, struct fuse_file_info *fi)
{
	(void)datasync;
	(void)fi;
	fuse_reply_err(req, 0);
}

static const struct cuse_lowlevel_ops cuse_ops = {
	.open = op_open,
	.read = op_read,
	.write = op_write,
	.ioctl = op_ioctl,
	.poll = op_poll,
	.flush = op_flush,
	.fsync = op_fsync,
	.release = op_release,
};

/* -------------------------------------------------------------------- setup */

/* The device node is created asynchronously by udev once the session is up, so
 * wait for it before reporting success.  Returns 0 when the node is there. */
static int wait_for_node(const struct cuse_tty_opts *opts, const char *path)
{
	int i;

	for (i = 0; i < 250 && access(path, F_OK) != 0; i++)
		usleep(20000); /* up to ~5 s */

	if (access(path, F_OK) != 0)
		return -ENODEV;

	if (opts->mode && chmod(path, opts->mode) != 0)
		log_warn("cuse: chmod(%s, %04o) failed: %s", path,
			 (unsigned)opts->mode, strerror(errno));

	if (opts->owner) {
		char *tmp = strdup(opts->owner);
		char *group = NULL;
		uid_t uid = (uid_t)-1;
		gid_t gid = (gid_t)-1;

		if (tmp) {
			char *colon = strchr(tmp, ':');

			if (colon) {
				*colon = '\0';
				group = colon + 1;
			}
			if (*tmp) {
				struct passwd *pw = getpwnam(tmp);

				if (pw) {
					uid = pw->pw_uid;
					gid = pw->pw_gid;
				} else {
					log_warn("cuse: unknown user '%s'", tmp);
				}
			}
			if (group && *group) {
				struct group *gr = getgrnam(group);

				if (gr)
					gid = gr->gr_gid;
				else
					log_warn("cuse: unknown group '%s'",
						 group);
			}
			free(tmp);
		}
		if ((uid != (uid_t)-1 || gid != (gid_t)-1) &&
		    chown(path, uid, gid) != 0)
			log_warn("cuse: chown(%s) failed: %s", path,
				 strerror(errno));
	}
	return 0;
}

static void *cuse_loop_thread(void *arg)
{
	struct cuse_tty *t = arg;

	/* Multithreaded loop: a write blocked on a full tx ring must not stall
	 * reads coming from the module. */
	fuse_session_loop_mt(t->se, 0);
	return NULL;
}

/*
 * fuse_session_exit() only sets a flag: libfuse's worker threads stay parked in
 * read() on /dev/cuse until that read is interrupted.  In a daemon whose
 * signals are consumed by sigwait() (so no thread ever sees EINTR), teardown
 * would block forever.  Deliver a harmless signal to every other thread to
 * make their reads return EINTR, at which point libfuse notices the exit flag.
 */
static void wake_handler(int signo)
{
	(void)signo;
}

static void interrupt_other_threads(void)
{
	DIR *dir = opendir("/proc/self/task");
	struct dirent *entry;
	pid_t self = getpid();
	pid_t me = (pid_t)syscall(SYS_gettid);

	if (!dir)
		return;
	while ((entry = readdir(dir)) != NULL) {
		char *end = NULL;
		long tid = strtol(entry->d_name, &end, 10);

		if (!end || *end || tid <= 0 || (pid_t)tid == me)
			continue;
		syscall(SYS_tgkill, self, (pid_t)tid, SIGUSR2);
	}
	closedir(dir);
}

static void install_wake_handler(void)
{
	struct sigaction sa;

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = wake_handler;
	sigemptyset(&sa.sa_mask);
	/* No SA_RESTART: the whole point is to make blocking reads fail with
	 * EINTR so the libfuse loop can observe the exit flag. */
	sa.sa_flags = 0;
	sigaction(SIGUSR2, &sa, NULL);
}

int cuse_tty_start(const struct cuse_tty_opts *opts, cuse_tty **out)
{
	struct cuse_tty *t;
	struct cuse_info ci;
	char devarg[96];
	const char *dev_info_argv[1];
	char *fargv[3];
	int multithreaded = 0;

	if (!opts || !opts->name || !*opts->name)
		return -EINVAL;

	/* A second daemon would silently take over the name and leave the first
	 * one unreachable, so refuse to start when the node is already there. */
	{
		char probe[80];
		struct stat sb;

		snprintf(probe, sizeof(probe), "/dev/%s", opts->name);
		if (stat(probe, &sb) == 0) {
			log_error("cuse: %s already exists - another chuart is "
				  "probably running (or use --name)", probe);
			return -EEXIST;
		}
	}

	t = calloc(1, sizeof(*t));
	if (!t)
		return -ENOMEM;

	t->opts = *opts;
	snprintf(t->name, sizeof(t->name), "%s", opts->name);
	snprintf(t->path, sizeof(t->path), "/dev/%s", opts->name);
	pthread_mutex_init(&t->lock, NULL);
	t->link_up = 1;
	/* Placeholder only: the client's tcsetattr is what reaches the module. */
	t->params.baud = PLACEHOLDER_BAUD;
	t->params.databits = 8;
	t->params.stopbits = 1;
	t->params.parity = 0;
	t->params.flow = 0;
	t->params.dtr = 0;
	t->params.rts = 0;
	t->params_known = 0;
	tty_build_termios(t);
	install_wake_handler();

	t->rx = ring_new(opts->rx_capacity ? opts->rx_capacity : 64 * 1024);
	t->tx = ring_new(opts->tx_capacity ? opts->tx_capacity : 64 * 1024);
	if (!t->rx || !t->tx) {
		cuse_tty_stop(t);
		return -ENOMEM;
	}

	snprintf(devarg, sizeof(devarg), "DEVNAME=%s", t->name);
	dev_info_argv[0] = devarg;
	memset(&ci, 0, sizeof(ci));
	ci.dev_info_argc = 1;
	ci.dev_info_argv = dev_info_argv;
	/* Required for TCGETS and friends: Linux does not encode a size for
	 * them, so the kernel cannot copy the argument on its own and we must
	 * request it with fuse_reply_ioctl_retry(). */
	ci.flags = CUSE_UNRESTRICTED_IOCTL;

	fargv[0] = (char *)"chuart";
	fargv[1] = (char *)"-f";
	fargv[2] = NULL;

	t->se = cuse_lowlevel_setup(2, fargv, &ci, &cuse_ops, &multithreaded, t);
	if (!t->se) {
		log_error("cuse: cannot set up /dev/%s - opening /dev/cuse needs "
			  "root and the cuse module", t->name);
		cuse_tty_stop(t);
		return -EACCES;
	}
	log_debug("cuse: session ready (libfuse multithreaded=%d)",
		  multithreaded);

	if (pthread_create(&t->thread, NULL, cuse_loop_thread, t) != 0) {
		int err = errno;

		log_error("cuse: cannot start request loop: %s", strerror(err));
		cuse_tty_stop(t);
		return -err;
	}
	t->thread_started = 1;

	/* Without the node the daemon is useless, so treat a missing node as a
	 * startup failure instead of reporting a device that is not there. */
	if (wait_for_node(&t->opts, t->path) != 0) {
		log_error("cuse: %s did not appear within 5 s (is udev running?)",
			  t->path);
		cuse_tty_stop(t);
		return -ENODEV;
	}
	log_info("cuse: %s ready", t->path);
	*out = t;
	return 0;
}

const char *cuse_tty_path(const cuse_tty *t)
{
	return t->path;
}

ring_t *cuse_tty_rx_ring(cuse_tty *t)
{
	return t->rx;
}

ring_t *cuse_tty_tx_ring(cuse_tty *t)
{
	return t->tx;
}

void cuse_tty_stop(cuse_tty *t)
{
	size_t i;

	if (!t)
		return;

	pthread_mutex_lock(&t->lock);
	t->stopping = 1;
	t->link_up = 0;
	for (i = 0; i < t->nreads; i++)
		fuse_reply_err(t->reads[i].req, EIO);
	t->nreads = 0;
	notify_pollers_locked(t);
	pthread_mutex_unlock(&t->lock);

	ring_close(t->rx);
	ring_close(t->tx);

	if (t->thread_started) {
		int joined = 0;

		fuse_session_exit(t->se);
		for (int attempt = 0; attempt < 600; attempt++) {
			if (pthread_tryjoin_np(t->thread, NULL) == 0) {
				joined = 1;
				break;
			}
			interrupt_other_threads();
			usleep(5000);
		}
		if (!joined) {
			/* Last resort: rather than hang forever holding the device
			 * node, exit and let the kernel reclaim the CUSE device when
			 * the file descriptors are closed. */
			log_error("cuse: request loop will not stop, aborting");
			_exit(EXIT_FAILURE);
		}
		t->thread_started = 0;
	}
	if (t->se) {
		cuse_lowlevel_teardown(t->se);
		t->se = NULL;
	}
	ring_free(t->rx);
	ring_free(t->tx);
	pthread_mutex_destroy(&t->lock);
	free(t);
}
