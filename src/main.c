#define _GNU_SOURCE
#include "ble.h"
#include "cuse_tty.h"
#include "log.h"
#include "ring.h"

#include <errno.h>
#include <getopt.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "ch9140lib.h"

#define CHUART_VERSION "0.1.0"
#define DEFAULT_DEVICE_NAME "ttyCH9140"
/* The library writes to 0xfff2 and configures through 0xfff3; the paired
 * notify characteristic is 0xfff1.  Use `chuart features` to confirm. */
#define DEFAULT_NOTIFY_UUID "0xfff1"
#define RX_RING_BYTES (256 * 1024)
#define TX_RING_BYTES (64 * 1024)

static const char *prog = "chuart";

static void usage(FILE *out)
{
	fprintf(out,
"usage: %s <command> [options]\n"
"\n"
"Bluetooth serial bridge for CH914X (CH9140lib) modules: scan for modules,\n"
"inspect their GATT tree, or expose a connected module as a /dev node usable\n"
"by picocom, minicom or a plain script.\n"
"\n"
"commands:\n"
"  scan                       list nearby CH914X modules\n"
"  features --mac <MAC>       connect and dump services/characteristics\n"
"  serve  --mac <MAC>         connect and expose /dev/%s\n"
"  version                    print library and adapter versions\n"
"  help                       this text\n"
"\n"
"common options:\n"
"  -v, --verbose              debug logging\n"
"  -q, --quiet                errors only\n"
"  -h, --help                 this text\n"
"\n"
"scan options:\n"
"  -t, --timeout <seconds>    scan duration (default 5)\n"
"      --json                 machine-readable output\n"
"\n"
"features options:\n"
"  -m, --mac <MAC>            module address\n"
"  -T, --connect-timeout <ms> default 15000\n"
"      --scan-time <seconds>  rescan before retrying a failed connect\n"
"                             (0 disables, default 5)\n"
"\n"
"serve options:\n"
"  -m, --mac <MAC>            module address\n"
"  -n, --name <name>          device name below /dev (default %s)\n"
"  -b, --baud <rate>          module UART baud rate (default 115200)\n"
"  -d, --data-bits <5..8>     default 8\n"
"  -s, --stop-bits <1|2>      default 1\n"
"  -p, --parity <none|odd|even|mark|space>   default none\n"
"  -f, --flow / -F, --no-flow module hardware flow control (default off)\n"
"  -D, --dtr  / -E, --no-dtr  assert DTR (default off)\n"
"  -R, --rts  / -S, --no-rts  assert RTS (default off)\n"
"  -u, --notify-uuid <uuid>   notify characteristic (default %s)\n"
"  -M, --mtu <n>              log an MTU override request\n"
"  -T, --connect-timeout <ms> default 15000\n"
"  -Y, --scan-time <seconds>  rescan before retrying a failed connect\n"
"                             (0 disables, default 5)\n"
"  -o, --owner <user[:group]> chown the device node\n"
"  -O, --mode <octal>         chmod the device node, e.g. 0660\n"
"\n"
"examples:\n"
"  %s scan -t 8\n"
"  %s features --mac 20:91:48:11:22:33\n"
"  sudo %s serve --mac 20:91:48:11:22:33 --baud 115200 --flow \\\n"
"       --owner $USER --mode 0660\n"
"  picocom --nolock -b 115200 /dev/%s\n"
"\n"
"note: creating the device node requires root, or a udev rule granting the\n"
"      user access to /dev/cuse.  See docs/ARCHITECTURE.md.\n",
		prog, DEFAULT_DEVICE_NAME, DEFAULT_DEVICE_NAME, DEFAULT_NOTIFY_UUID,
		prog, prog, prog, DEFAULT_DEVICE_NAME);
}

static int parse_int(const char *s, int min, int max, int *out)
{
	char *end = NULL;
	long v;

	if (!s || !*s)
		return -1;
	v = strtol(s, &end, 0);
	if ((end && *end) || v < min || v > max)
		return -1;
	*out = (int)v;
	return 0;
}

static int parse_parity(const char *s, int *out)
{
	static const struct {
		const char *name;
		int value;
	} map[] = {
		{ "none", 0 }, { "odd", 1 }, { "even", 2 }, { "mark", 3 },
		{ "space", 4 },
	};

	if (!s || !*s)
		return -1;
	for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
		if (strcmp(s, map[i].name) == 0 || (s[1] == '\0' && s[0] == map[i].name[0])) {
			*out = map[i].value;
			return 0;
		}
	}
	return -1;
}

static const char *parity_name(int parity)
{
	static const char *names[] = { "none", "odd", "even", "mark", "space" };

	return parity >= 0 && parity <= 4 ? names[parity] : "?";
}

/* -------------------------------------------------------------------- scan --- */

struct scan_state {
	int json;
	struct ble_device *devs;
	size_t count;
	size_t capacity;
};

static void print_json_string(const char *s)
{
	putchar('"');
	for (; *s; s++) {
		if (*s == '"' || *s == '\\')
			printf("\\%c", *s);
		else if ((unsigned char)*s < 0x20)
			printf("\\u%04x", *s);
		else
			putchar(*s);
	}
	putchar('"');
}

static void print_device_line(const struct ble_device *dev)
{
	printf("%-18s %-32s %5d dBm  chip_ver=%u (0x%02x)\n", dev->addr,
	       dev->name[0] ? dev->name : "(no name)", dev->rssi, dev->chip_ver,
	       dev->chip_ver);
}

/* The library reports every advertisement it sees, so the same module arrives
 * many times: keep one entry per address and remember the best signal. */
static void on_scan_device(const struct ble_device *dev, void *user)
{
	struct scan_state *st = user;

	for (size_t i = 0; i < st->count; i++) {
		if (strcmp(st->devs[i].addr, dev->addr) != 0)
			continue;
		if (dev->rssi > st->devs[i].rssi)
			st->devs[i].rssi = dev->rssi;
		if (!st->devs[i].name[0] && dev->name[0])
			snprintf(st->devs[i].name, sizeof(st->devs[i].name), "%s",
				 dev->name);
		return;
	}

	if (st->count == st->capacity) {
		size_t cap = st->capacity ? st->capacity * 2 : 8;
		struct ble_device *grown = realloc(st->devs, cap * sizeof(*grown));

		if (!grown) {
			log_warn("out of memory, device list truncated");
			return;
		}
		st->devs = grown;
		st->capacity = cap;
	}
	st->devs[st->count++] = *dev;
	if (!st->json)
		print_device_line(dev);
}

static int cmp_rssi_desc(const void *a, const void *b)
{
	const struct ble_device *da = a;
	const struct ble_device *db = b;

	return (int)db->rssi - (int)da->rssi;
}

static int cmd_scan(int argc, char **argv)
{
	static const struct option opts[] = {
		{ "timeout", required_argument, NULL, 't' },
		{ "json", no_argument, NULL, 'j' },
		{ "verbose", no_argument, NULL, 'v' },
		{ "quiet", no_argument, NULL, 'q' },
		{ "help", no_argument, NULL, 'h' },
		{ NULL, 0, NULL, 0 },
	};
	struct scan_state st = { 0 };
	int seconds = 5;
	int opt;

	optind = 1;
	while ((opt = getopt_long(argc, argv, "t:jvqh", opts, NULL)) != -1) {
		switch (opt) {
		case 't':
			if (parse_int(optarg, 1, 600, &seconds) != 0) {
				log_error("invalid scan time '%s'", optarg);
				return 2;
			}
			break;
		case 'j':
			st.json = 1;
			break;
		case 'v':
			log_set_level(LOG_LEVEL_DEBUG);
			break;
		case 'q':
			log_set_level(LOG_LEVEL_ERROR);
			break;
		case 'h':
			usage(stdout);
			return 0;
		default:
			return 2;
		}
	}

	if (!st.json)
		printf("%-18s %-32s %9s  %s\n", "ADDRESS", "NAME", "RSSI", "CHIP");

	int rc = ble_scan(seconds, on_scan_device, &st);

	if (st.json) {
		qsort(st.devs, st.count, sizeof(st.devs[0]), cmp_rssi_desc);
		printf("[\n");
		for (size_t i = 0; i < st.count; i++) {
			printf("%s  {\"addr\": ", i ? "," : "");
			print_json_string(st.devs[i].addr);
			printf(", \"name\": ");
			print_json_string(st.devs[i].name);
			printf(", \"rssi\": %d, \"chip_ver\": %u}\n",
			       st.devs[i].rssi, st.devs[i].chip_ver);
		}
		printf("]\n");
	}
	free(st.devs);
	if (rc != 0)
		return 1;
	if (st.count == 0)
		log_warn("no CH914X module found nearby");
	else
		log_info("found %zu module(s)", st.count);
	return 0;
}

/* ---------------------------------------------------------------- features --- */

static void print_properties(uint8_t props)
{
	static const struct {
		uint8_t bit;
		const char *name;
	} bits[] = {
		{ 0x01, "broadcast" }, { 0x02, "read" },
		{ 0x04, "write-nr" },  { 0x08, "write" },
		{ 0x10, "notify" },    { 0x20, "indicate" },
	};

	for (size_t i = 0; i < sizeof(bits) / sizeof(bits[0]); i++)
		if (props & bits[i].bit)
			printf("%s ", bits[i].name);
}

static const char *uuid_role(const char *uuid)
{
	if (strcasecmp(uuid, "0xfff1") == 0)
		return "  <- notify/data RX, chuart's default --notify-uuid";
	if (strcasecmp(uuid, "0xfff2") == 0)
		return "  <- data/TX, hardcoded by CH9140lib";
	if (strcasecmp(uuid, "0xfff3") == 0)
		return "  <- config baud/modem, hardcoded by CH9140lib";
	return "";
}

static int cmd_features(int argc, char **argv)
{
	static const struct option opts[] = {
		{ "mac", required_argument, NULL, 'm' },
		{ "connect-timeout", required_argument, NULL, 'T' },
		{ "scan-time", required_argument, NULL, 'Y' },
		{ "verbose", no_argument, NULL, 'v' },
		{ "quiet", no_argument, NULL, 'q' },
		{ "help", no_argument, NULL, 'h' },
		{ NULL, 0, NULL, 0 },
	};
	const char *mac = NULL;
	int connect_timeout = 15000;
	int scan_time = 5;
	int opt;

	optind = 1;
	while ((opt = getopt_long(argc, argv, "m:T:Y:vqh", opts, NULL)) != -1) {
		switch (opt) {
		case 'm':
			mac = optarg;
			break;
		case 'T':
			if (parse_int(optarg, 100, 600000, &connect_timeout) != 0) {
				log_error("invalid connect timeout '%s'", optarg);
				return 2;
			}
			break;
		case 'Y':
			if (parse_int(optarg, 0, 600, &scan_time) != 0) {
				log_error("invalid scan time '%s'", optarg);
				return 2;
			}
			break;
		case 'v':
			log_set_level(LOG_LEVEL_DEBUG);
			break;
		case 'q':
			log_set_level(LOG_LEVEL_ERROR);
			break;
		case 'h':
			usage(stdout);
			return 0;
		default:
			return 2;
		}
	}
	if (!mac) {
		log_error("features requires --mac <MAC>");
		return 2;
	}

	int version = 0;
	const char *version_name = NULL;
	int rc = ble_adapter_check(&version, &version_name);

	if (rc != 0) {
		log_error("bluetooth adapter unusable: %s", strerror(-rc));
		return 1;
	}
	printf("adapter bluetooth version: %s (%d)\n", version_name, version);

	struct ble_link_opts lopts = {
		.notify_uuid = DEFAULT_NOTIFY_UUID,
		.connect_timeout_ms = connect_timeout,
		.pre_scan_seconds = scan_time,
		.disconnect_quiesce_ms = 200,
	};
	struct ble_link *link = ble_link_open(mac, &lopts);

	if (!link)
		return 1;

	CH9140HANDLE *h = ble_link_handle(link);
	uint16_t mtu = 0;

	if (CH9140Ble_Get_MTU(h, &mtu) == 0)
		printf("MTU: %u\n", mtu);
	else
		printf("MTU: unknown\n");

	gatt_primary_service services[64];
	int nservices = (int)(sizeof(services) / sizeof(services[0]));

	/* CH9140lib only fills the entries it manages to resolve, so the caller's
	 * arrays must start zeroed or they end up holding stack garbage. */
	memset(services, 0, sizeof(services));

	rc = CH9140Ble_discover_primary(h, services, &nservices);
	if (rc != 0 || nservices <= 0) {
		log_error("service discovery failed (rc=%d, n=%d)", rc, nservices);
		ble_link_close(link);
		return 1;
	}

	printf("services: %d\n", nservices);
	for (int i = 0; i < nservices; i++) {
		char suuid[64] = { 0 };
		char handle[16];

		gatt_uuid_to_string(&services[i].uuid, suuid, sizeof(suuid));
		snprintf(handle, sizeof(handle), "%04x",
			 services[i].attr_handle_start);
		printf("\n  service 0x%s  %s  (handles 0x%04x-0x%04x)\n", handle,
		       suuid, services[i].attr_handle_start,
		       services[i].attr_handle_end);

		gatt_characteristic chars[64];

		memset(chars, 0, sizeof(chars));
		int nchars = (int)(sizeof(chars) / sizeof(chars[0]));

		if (CH9140Ble_discover_characteristics(h, handle, chars,
						       &nchars) != 0) {
			printf("    (characteristic discovery failed)\n");
			continue;
		}

		/* Only the vendor's data service is relevant for --notify-uuid, so
		 * keep the hint there instead of flagging every notify-capable
		 * characteristic in the device. */
		int data_service = 0;
		int unresolved = 0;

		for (int c = 0; c < nchars; c++) {
			char cuuid[64] = { 0 };

			if (gatt_uuid_to_string(&chars[c].uuid, cuuid,
						sizeof(cuuid)) != 0 ||
			    strcmp(cuuid, "00000000-0000-0000-0000-000000000000") == 0) {
				unresolved++;
				continue;
			}
			if (strcasecmp(cuuid, "0xfff2") == 0 ||
			    strcasecmp(cuuid, "0xfff1") == 0)
				data_service = 1;
		}

		for (int c = 0; c < nchars; c++) {
			char cuuid[64] = { 0 };

			if (gatt_uuid_to_string(&chars[c].uuid, cuuid,
						sizeof(cuuid)) != 0 ||
			    strcmp(cuuid, "00000000-0000-0000-0000-000000000000") == 0)
				continue;
			printf("    0x%04x  %-40s props=0x%02x  [ ",
			       chars[c].handle, cuuid, chars[c].properties);
			print_properties(chars[c].properties);
			printf("]%s\n", uuid_role(cuuid));
			if (data_service && (chars[c].properties & 0x10))
				printf("            write channel: --notify-uuid %s\n",
				       cuuid);
		}
		if (unresolved)
			printf("    (%d characteristic(s) not resolved by CH9140lib)\n",
			       unresolved);
	}
	printf("\n");
	ble_link_close(link);
	return 0;
}

/* ------------------------------------------------------------------- serve --- */

struct writer_ctx {
	struct ble_link *link;
	ring_t *tx;
	struct cuse_tty *tty;
	volatile sig_atomic_t stop;
	volatile sig_atomic_t link_failed;
	pthread_t thread;
	int started;
	unsigned long sent;
};

static void wake_supervisor(void);

static void on_rx_ready(void *user)
{
	cuse_tty_rx_ready((cuse_tty *)user);
}

static void *writer_thread(void *arg)
{
	struct writer_ctx *w = arg;
	unsigned char buf[4096];

	while (!w->stop) {
		ssize_t n = ring_read(w->tx, buf, sizeof(buf), 200);

		if (n < 0) {
			log_debug("writer: tx ring closed");
			break;
		}
		if (n == 0)
			continue;

		ssize_t sent = ble_link_write(w->link, buf, (size_t)n);

		if (sent < 0) {
			log_error("writer: link failed while sending");
			cuse_tty_link_lost(w->tty, "write failed");
			w->link_failed = 1;
			wake_supervisor();
			break;
		}
		log_debug("writer: sent %zd byte(s) to the module", sent);
		w->sent += (unsigned long)sent;
		if (ring_count(w->tx) < ring_capacity(w->tx))
			cuse_tty_tx_space(w->tty);
	}
	return NULL;
}

static volatile sig_atomic_t g_link_lost;

static void wake_supervisor(void)
{
	kill(getpid(), SIGUSR1);
}

static void on_link_lost(void *user, const char *reason)
{
	(void)user;
	(void)reason;
	g_link_lost = 1;
	wake_supervisor();
}

static int cmd_serve(int argc, char **argv)
{
	static const struct option opts[] = {
		{ "mac", required_argument, NULL, 'm' },
		{ "name", required_argument, NULL, 'n' },
		{ "baud", required_argument, NULL, 'b' },
		{ "data-bits", required_argument, NULL, 'd' },
		{ "stop-bits", required_argument, NULL, 's' },
		{ "parity", required_argument, NULL, 'p' },
		{ "flow", no_argument, NULL, 'f' },
		{ "no-flow", no_argument, NULL, 'F' },
		{ "dtr", no_argument, NULL, 'D' },
		{ "no-dtr", no_argument, NULL, 'E' },
		{ "rts", no_argument, NULL, 'R' },
		{ "no-rts", no_argument, NULL, 'S' },
		{ "notify-uuid", required_argument, NULL, 'u' },
		{ "mtu", required_argument, NULL, 'M' },
		{ "connect-timeout", required_argument, NULL, 'T' },
		{ "scan-time", required_argument, NULL, 'Y' },
		{ "owner", required_argument, NULL, 'o' },
		{ "mode", required_argument, NULL, 'O' },
		{ "verbose", no_argument, NULL, 'v' },
		{ "quiet", no_argument, NULL, 'q' },
		{ "help", no_argument, NULL, 'h' },
		{ NULL, 0, NULL, 0 },
	};
	struct {
		const char *mac;
		const char *name;
		const char *notify_uuid;
		const char *owner;
		int baud;
		int databits;
		int stopbits;
		int parity;
		int flow;
		int dtr;
		int rts;
		int mtu;
		int connect_timeout;
		int scan_time;
		mode_t mode;
	} o = {
		.name = DEFAULT_DEVICE_NAME,
		.notify_uuid = DEFAULT_NOTIFY_UUID,
		.baud = 115200,
		.databits = 8,
		.stopbits = 1,
		.parity = 0,
		.connect_timeout = 15000,
		.scan_time = 5,
		.mode = 0,
	};
	int opt;

	optind = 1;
	while ((opt = getopt_long(argc, argv, "m:n:b:d:s:p:fFDERSu:M:T:Y:o:O:vqh",
				  opts, NULL)) != -1) {
		switch (opt) {
		case 'm':
			o.mac = optarg;
			break;
		case 'n':
			o.name = optarg;
			break;
		case 'b':
			if (parse_int(optarg, 0, 4000000, &o.baud) != 0) {
				log_error("invalid baud rate '%s'", optarg);
				return 2;
			}
			break;
		case 'd':
			if (parse_int(optarg, 5, 8, &o.databits) != 0) {
				log_error("invalid data bits '%s'", optarg);
				return 2;
			}
			break;
		case 's':
			if (parse_int(optarg, 1, 2, &o.stopbits) != 0) {
				log_error("invalid stop bits '%s'", optarg);
				return 2;
			}
			break;
		case 'p':
			if (parse_parity(optarg, &o.parity) != 0) {
				log_error("invalid parity '%s' (none|odd|even|mark|space)",
					  optarg);
				return 2;
			}
			break;
		case 'f':
			o.flow = 1;
			break;
		case 'F':
			o.flow = 0;
			break;
		case 'D':
			o.dtr = 1;
			break;
		case 'E':
			o.dtr = 0;
			break;
		case 'R':
			o.rts = 1;
			break;
		case 'S':
			o.rts = 0;
			break;
		case 'u':
			o.notify_uuid = optarg;
			break;
		case 'M':
			if (parse_int(optarg, 23, 4096, &o.mtu) != 0) {
				log_error("invalid MTU '%s'", optarg);
				return 2;
			}
			break;
		case 'T':
			if (parse_int(optarg, 100, 600000, &o.connect_timeout) != 0) {
				log_error("invalid connect timeout '%s'", optarg);
				return 2;
			}
			break;
		case 'Y':
			if (parse_int(optarg, 0, 600, &o.scan_time) != 0) {
				log_error("invalid scan time '%s'", optarg);
				return 2;
			}
			break;
		case 'o':
			o.owner = optarg;
			break;
		case 'O': {
			int mode = 0;

			if (parse_int(optarg, 0, 07777, &mode) != 0) {
				log_error("invalid mode '%s'", optarg);
				return 2;
			}
			o.mode = (mode_t)mode;
			break;
		}
		case 'v':
			log_set_level(LOG_LEVEL_DEBUG);
			break;
		case 'q':
			log_set_level(LOG_LEVEL_ERROR);
			break;
		case 'h':
			usage(stdout);
			return 0;
		default:
			return 2;
		}
	}

	if (!o.mac) {
		log_error("serve requires --mac <MAC>");
		return 2;
	}

	/* Block the signals we handle so every thread inherits the mask and only
	 * the supervisor ever sees them. */
	sigset_t set;

	sigemptyset(&set);
	sigaddset(&set, SIGINT);
	sigaddset(&set, SIGTERM);
	sigaddset(&set, SIGUSR1);
	pthread_sigmask(SIG_BLOCK, &set, NULL);
	signal(SIGPIPE, SIG_IGN);

	struct cuse_tty_opts topts = {
		.name = o.name,
		.owner = o.owner,
		.mode = o.mode,
		.baud = o.baud,
		.databits = o.databits,
		.stopbits = o.stopbits,
		.parity = o.parity,
		.flow = o.flow,
		.rx_capacity = RX_RING_BYTES,
		.tx_capacity = TX_RING_BYTES,
	};

	struct cuse_tty *tty = NULL;

	if (cuse_tty_start(&topts, &tty) != 0)
		return 1;

	struct ble_link_opts lopts = {
		.notify_uuid = o.notify_uuid,
		.connect_timeout_ms = o.connect_timeout,
		.pre_scan_seconds = o.scan_time,
		.disconnect_quiesce_ms = 200,
		.rx = cuse_tty_rx_ring(tty),
		.rx_hook = on_rx_ready,
		.link_lost = on_link_lost,
		.user = tty,
	};

	struct ble_link *link = ble_link_open(o.mac, &lopts);

	if (!link) {
		cuse_tty_stop(tty);
		return 1;
	}

	if (ble_link_startup(link, o.baud, o.databits, o.stopbits, o.parity,
			     o.flow, o.dtr, o.rts) != 0) {
		ble_link_close(link);
		cuse_tty_stop(tty);
		return 1;
	}
	if (o.mtu > 0)
		log_info("ble: MTU override requested (%d), module reports %u", o.mtu,
			 ble_link_mtu(link));

	struct writer_ctx writer = {
		.link = link,
		.tx = cuse_tty_tx_ring(tty),
		.tty = tty,
	};

	if (pthread_create(&writer.thread, NULL, writer_thread, &writer) != 0) {
		log_error("cannot start the writer thread: %s", strerror(errno));
		ble_link_close(link);
		cuse_tty_stop(tty);
		return 1;
	}
	writer.started = 1;

	log_info("device ready: %s (MTU %u, %d %d%s%d)", cuse_tty_path(tty),
		 ble_link_mtu(link), o.baud, o.databits, parity_name(o.parity),
		 o.stopbits);
	printf("device ready: %s\n", cuse_tty_path(tty));
	printf("try: picocom --nolock -b %d %s\n", o.baud, cuse_tty_path(tty));
	printf("press Ctrl-C to stop\n");
	fflush(stdout);

	int exit_code = 0;

	for (;;) {
		int sig = 0;

		if (sigwait(&set, &sig) != 0)
			continue;
		if (sig == SIGINT || sig == SIGTERM) {
			log_info("signal %d received, shutting down", sig);
			break;
		}
		if (g_link_lost || writer.link_failed) {
			log_warn("the BLE link is gone, shutting down");
			cuse_tty_link_lost(tty, "module disconnected");
			exit_code = 1;
			break;
		}
	}

	writer.stop = 1;
	if (writer.started)
		pthread_join(writer.thread, NULL);
	log_info("forwarded %lu byte(s) to the module", writer.sent);
	ble_link_close(link);
	cuse_tty_stop(tty);
	log_info("stopped");
	return exit_code;
}

/* ----------------------------------------------------------------- version --- */

static int cmd_version(void)
{
	int version = 0;
	const char *name = NULL;
	int rc = ble_adapter_check(&version, &name);

	printf("chuart      %s\n", CHUART_VERSION);
	printf("CH9140lib   libCH9140lib.so\n");
	if (rc == 0)
		printf("bluetooth   %s (%d)\n", name, version);
	else if (rc == -EPERM)
		printf("bluetooth   adapter present but powered off\n");
	else
		printf("bluetooth   no usable adapter (%s)\n", strerror(-rc));
	return rc == 0 ? 0 : 1;
}

/* -------------------------------------------------------------------- main --- */

int main(int argc, char **argv)
{
	if (argc > 0 && argv[0] && argv[0][0])
		prog = argv[0];

	if (argc < 2) {
		usage(stderr);
		return 2;
	}

	const char *cmd = argv[1];
	int sub_argc = argc - 1;
	char **sub_argv = argv + 1;

	if (strcmp(cmd, "scan") == 0)
		return cmd_scan(sub_argc, sub_argv);
	if (strcmp(cmd, "features") == 0 || strcmp(cmd, "list-features") == 0)
		return cmd_features(sub_argc, sub_argv);
	if (strcmp(cmd, "serve") == 0)
		return cmd_serve(sub_argc, sub_argv);
	if (strcmp(cmd, "version") == 0 || strcmp(cmd, "--version") == 0)
		return cmd_version();
	if (strcmp(cmd, "help") == 0 || strcmp(cmd, "--help") == 0 ||
	    strcmp(cmd, "-h") == 0) {
		usage(stdout);
		return 0;
	}

	log_error("unknown command '%s'", cmd);
	usage(stderr);
	return 2;
}
