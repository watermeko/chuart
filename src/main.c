#define _GNU_SOURCE
#include "ble.h"
#include "cuse_tty.h"
#include "devices.h"
#include "log.h"
#include "ring.h"
#include "ui.h"

#include <errno.h>
#include <getopt.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
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
"  scan                       list nearby CH914X modules (assigns names)\n"
"  devices                    list the modules chuart remembers\n"
"  rename <name|mac> <new>    give a module a name of your own\n"
"  features <--mac|--device>  connect and dump services/characteristics\n"
"  serve    <--mac|--device>  connect and expose /dev/%s\n"
"  version                    print library and adapter versions\n"
"  help                       this text\n"
"\n"
"Every module gets a memorable name the first time it is seen, e.g.\n"
"\"excitedApple\", stored in %s.  Use that name instead\n"
"of a MAC address:\n"
"\n"
"  chuart scan                    # shows the assigned names\n"
"  chuart serve -d excitedApple   # same as --mac 50:54:7B:...\n"
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
"  -d, --device <name>        module by name (see `chuart devices`)\n"
"  -m, --mac <MAC>            module by address\n"
"  -T, --connect-timeout <ms> default 15000\n"
"      --scan-time <seconds>  rescan before retrying a failed connect\n"
"                             (0 disables, default 5)\n"
"\n"
"serve options:\n"
"  -d, --device <name>        module by name (see `chuart devices`)\n"
"  -m, --mac <MAC>            module by address\n"
"  -n, --name <name>          device name below /dev (default %s)\n"
"serial parameters are not configured here: the port is a pure mapping of the\n"
"module, so the client owns them (picocom -b 9600 /dev/<name> really retunes the\n"
"module UART).  Until a client sets them, the module keeps whatever it has.\n"
"  -u, --notify-uuid <uuid>   notify characteristic (default %s)\n"
"  -M, --mtu <n>              cap the MTU used for writes (default: the\n"
"                             value negotiated with the module)\n"
"  -T, --connect-timeout <ms> default 15000\n"
"  -Y, --scan-time <seconds>  rescan before retrying a failed connect\n"
"                             (0 disables, default 5)\n"
"  -o, --owner <user[:group]> chown the device node\n"
"  -O, --mode <octal>         chmod the device node, e.g. 0660\n"

"\n"
"examples:\n"
"  %s scan -t 8\n"
"  %s devices\n"
"  %s rename excitedApple lab-rig\n"
"  %s features --mac 20:91:48:11:22:33\n"
"  sudo %s serve --mac 20:91:48:11:22:33 --owner $USER --mode 0660\n"
"  picocom --nolock -b 115200 /dev/%s\n"
"\n"
"note: creating the device node requires root, or a udev rule granting the\n"
"      user access to /dev/cuse.  See the troubleshooting section in README.md.\n",
		prog, DEFAULT_DEVICE_NAME, devices_path(), DEFAULT_DEVICE_NAME,
		DEFAULT_NOTIFY_UUID, prog, prog, prog, prog, prog,
		DEFAULT_DEVICE_NAME);
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

/*
 * Turn a --device <name> or --mac <MAC> selection into a MAC address, and make
 * sure the module has a name in the registry either way.
 */
static int resolve_device(const char *device, const char *mac, char *mac_out,
			  size_t mac_size, char *name_out, size_t name_size)
{
	struct devices *d;
	char resolved[DEVICE_MAC_MAX] = { 0 };
	int rc;

	if (!device && !mac) {
		log_error("specify the module with --device <name> or --mac <MAC>");
		return -1;
	}
	if (device && mac) {
		log_error("use either --device or --mac, not both");
		return -1;
	}

	d = devices_open();
	if (!d)
		return -1;

	if (mac) {
		snprintf(resolved, sizeof(resolved), "%s", mac);
		if (devices_lookup(d, mac, resolved) != 0) {
			/* Not a MAC either: maybe it is a name after all. */
			if (devices_lookup(d, mac, resolved) != 0) {
				log_error("'%s' is neither a MAC address nor a known "
					  "device name (see `chuart devices`)", mac);
				devices_close(d);
				return -1;
			}
		}
	} else {
		if (devices_lookup(d, device, resolved) != 0) {
			log_error("unknown device '%s' (see `chuart devices`, or pass "
				  "--mac)", device);
			devices_close(d);
			return -1;
		}
	}

	char name[DEVICE_NAME_MAX] = "";
	int created = 0;

	rc = devices_touch(d, resolved, NULL, name, &created);
	if (rc != 0) {
		/* Naming is a convenience: never refuse to connect without one. */
		if (rc == -ENOSPC)
			log_warn("no free name is left for this module; continuing "
				 "with the MAC address (see `chuart devices`)");
		else
			log_warn("not naming this module (%s); continuing with the "
				 "MAC address", strerror(-rc));
		name[0] = '\0';
	} else if (devices_save(d) != 0) {
		log_warn("cannot save device names to %s", devices_path());
	}
	devices_close(d);

	if (created)
		log_info("this module is now called '%s' (rename with `chuart "
			 "rename %s <new-name>`)", name, name);

	snprintf(mac_out, mac_size, "%s", resolved);
	if (name_out && name_size)
		snprintf(name_out, name_size, "%s", name);
	return 0;
}

/* -------------------------------------------------------------------- scan --- */

struct scan_state {
	int json;
	struct devices *registry;
	struct ble_device *devs;
	char (*names)[DEVICE_NAME_MAX]; /* friendly name per device */
	int warned_full;
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
		char (*names)[DEVICE_NAME_MAX] =
			realloc(st->names, cap * sizeof(*st->names));

		if (!grown || !names) {
			free(grown);
			free(names);
			log_warn("out of memory, device list truncated");
			return;
		}
		st->devs = grown;
		st->names = names;
		st->capacity = cap;
	}
	st->devs[st->count] = *dev;
	st->names[st->count][0] = '\0';
	if (st->registry) {
		int rc = devices_touch(st->registry, dev->addr, dev->name,
				       st->names[st->count], NULL);

		if (rc == -ENOSPC && !st->warned_full) {
			log_warn("no free name is left (%d combinations in use); "
				 "rename or remove entries in %s",
				 10 * 10, devices_path());
			st->warned_full = 1;
		}
	}
	st->count++;
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

	{
		struct ble_adapter_status ad;
		char why[256];

		if (ble_adapter_check(&ad, why, sizeof(why)) != 0) {
			log_error("bluetooth unusable: %s", why);
			return 1;
		}
	}

	st.registry = devices_open();
	if (!st.json)
		fprintf(stderr, "%sScanning for nearby modules (%d s)...%s\n",
			ui_ec("2"), seconds, ui_ec("0"));

	int rc = ble_scan(seconds, on_scan_device, &st);
	if (st.registry && devices_save(st.registry) != 0)
		log_warn("cannot save device names to %s", devices_path());

	/* Strongest signal first, in both output modes. */
	size_t *order = calloc(st.count ? st.count : 1, sizeof(*order));

	if (!order) {
		log_error("out of memory");
		free(st.devs);
		free(st.names);
		devices_close(st.registry);
		return 1;
	}
	for (size_t i = 0; i < st.count; i++)
		order[i] = i;
	for (size_t i = 1; i < st.count; i++) {
		size_t key = order[i];
		size_t j = i;

		while (j > 0 && st.devs[order[j - 1]].rssi < st.devs[key].rssi) {
			order[j] = order[j - 1];
			j--;
		}
		order[j] = key;
	}

	if (st.json) {
		printf("[\n");
		for (size_t n = 0; n < st.count; n++) {
			size_t i = order[n];

			printf("%s  {\"device\": ", n ? "," : "");
			print_json_string(st.names[i]);
			printf(", \"addr\": ");
			print_json_string(st.devs[i].addr);
			printf(", \"module\": ");
			print_json_string(st.devs[i].name);
			printf(", \"rssi\": %d, \"chip_ver\": %u}\n",
			       st.devs[i].rssi, st.devs[i].chip_ver);
		}
		printf("]\n");
	} else if (st.count > 0) {
		static const struct ui_col cols[] = {
			{ "DEVICE", 0 }, { "ADDRESS", 0 }, { "MODULE", 0 },
			{ "RSSI", 1 },   { "CHIP", 0 },
		};
		struct ui_cell *cells = calloc(st.count * 5, sizeof(*cells));
		char (*rssi)[16] = calloc(st.count, sizeof(*rssi));
		char (*chip)[16] = calloc(st.count, sizeof(*chip));

		if (cells && rssi && chip) {
			for (size_t n = 0; n < st.count; n++) {
				size_t i = order[n];
				struct ui_cell *row = &cells[n * 5];

				snprintf(rssi[n], sizeof(rssi[n]), "%d dBm",
					 st.devs[i].rssi);
				snprintf(chip[n], sizeof(chip[n]), "0x%02x",
					 st.devs[i].chip_ver);
				row[0].text = st.names[i][0] ? st.names[i] : "-";
				row[0].sgr = ui_c("1;36");
				row[1].text = st.devs[i].addr;
				row[1].sgr = UI_DIM;
				row[2].text = st.devs[i].name[0]
						      ? st.devs[i].name
						      : "-";
				row[3].text = rssi[n];
				row[3].sgr = ui_rssi_color(st.devs[i].rssi);
				row[4].text = chip[n];
				row[4].sgr = UI_DIM;
			}
			ui_table(cols, 5, cells, st.count);
		}
		free(cells);
		free(rssi);
		free(chip);
	}
	free(order);
	free(st.devs);
	free(st.names);
	devices_close(st.registry);

	if (rc != 0)
		return 1;

	if (!st.json) {
		char path[256];

		if (st.count == 0)
			fprintf(stderr, "%sNo CH914X module found nearby.%s\n",
				ui_ec("33"), ui_ec("0"));
		else
			fprintf(stderr, "%s%zu module(s) - names in %s%s\n",
				ui_ec("2"), st.count,
				ui_short_home(devices_path(), path, sizeof(path)),
				ui_ec("0"));
	}
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

/* Rendered in dim; the role is a hint next to the characteristic. */
static const char *uuid_role(const char *uuid)
{
	if (strcasecmp(uuid, "0xfff1") == 0)
		return "  <- notify/data RX (chuart's default --notify-uuid)";
	if (strcasecmp(uuid, "0xfff2") == 0)
		return "  <- data/TX, hardcoded by CH9140lib";
	if (strcasecmp(uuid, "0xfff3") == 0)
		return "  <- config baud/modem, hardcoded by CH9140lib";
	return "";
}

static int cmd_features(int argc, char **argv)
{
	static const struct option opts[] = {
		{ "device", required_argument, NULL, 'd' },
		{ "mac", required_argument, NULL, 'm' },
		{ "connect-timeout", required_argument, NULL, 'T' },
		{ "scan-time", required_argument, NULL, 'Y' },
		{ "verbose", no_argument, NULL, 'v' },
		{ "quiet", no_argument, NULL, 'q' },
		{ "help", no_argument, NULL, 'h' },
		{ NULL, 0, NULL, 0 },
	};
	const char *device = NULL;
	const char *mac = NULL;
	int connect_timeout = 15000;
	int scan_time = 5;
	int opt;

	optind = 1;
	while ((opt = getopt_long(argc, argv, "d:m:T:Y:vqh", opts, NULL)) != -1) {
		switch (opt) {
		case 'd':
			device = optarg;
			break;
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
	char mac_buf[DEVICE_MAC_MAX];
	char name_buf[DEVICE_NAME_MAX];

	if (resolve_device(device, mac, mac_buf, sizeof(mac_buf), name_buf,
			   sizeof(name_buf)) != 0)
		return 2;
	mac = mac_buf;
	log_info("device '%s' (%s)", name_buf, mac_buf);

	struct ble_adapter_status ad;
	char why[256];
	int rc = ble_adapter_check(&ad, why, sizeof(why));

	if (rc != 0) {
		log_error("bluetooth unusable: %s", why);
		return 1;
	}
	printf("%sadapter%s %s, bluetooth %s (%d)\n", UI_DIM, UI_RESET,
	       ad.adapter, ad.version_name, ad.version);

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
		printf("%sMTU%s %u\n", UI_DIM, UI_RESET, mtu);
	else
		printf("%sMTU%s unknown\n", UI_DIM, UI_RESET);

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

	printf("%sservices%s %d\n", UI_DIM, UI_RESET, nservices);
	for (int i = 0; i < nservices; i++) {
		char suuid[64] = { 0 };
		char handle[16];

		gatt_uuid_to_string(&services[i].uuid, suuid, sizeof(suuid));
		snprintf(handle, sizeof(handle), "%04x",
			 services[i].attr_handle_start);
		printf("\n  %sservice%s %s%s%s  %s  %s(0x%04x-0x%04x)%s\n",
		       UI_BOLD, UI_RESET, UI_BOLD, handle, UI_RESET, suuid,
		       UI_DIM, services[i].attr_handle_start,
		       services[i].attr_handle_end, UI_RESET);

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
			printf("    %s0x%04x%s  %-40s %sprops=0x%02x  [%s ",
			       UI_DIM, chars[c].handle, UI_RESET, cuuid, UI_DIM,
			       chars[c].properties, UI_RESET);
			print_properties(chars[c].properties);
			printf("%s ]%s%s\n", UI_DIM,
			       uuid_role(cuuid), UI_RESET);
			if (data_service && (chars[c].properties & 0x10) &&
			    strcasecmp(cuuid, DEFAULT_NOTIFY_UUID) == 0)
				printf("            %sthis is what chuart uses by "
				       "default (%s)%s\n", UI_CYAN,
				       DEFAULT_NOTIFY_UUID, UI_RESET);
		}
		if (unresolved)
			printf("    %s(%d characteristic(s) not resolved by "
			       "CH9140lib)%s\n", UI_DIM, unresolved, UI_RESET);
	}
	printf("\n");
	ble_link_close(link);
	return 0;
}

/* ------------------------------------------------------------------ devices --- */

static int cmd_devices(int argc, char **argv)
{
	static const struct option opts[] = {
		{ "json", no_argument, NULL, 'j' },
		{ "verbose", no_argument, NULL, 'v' },
		{ "quiet", no_argument, NULL, 'q' },
		{ "help", no_argument, NULL, 'h' },
		{ NULL, 0, NULL, 0 },
	};
	int json = 0;
	int opt;

	optind = 1;
	while ((opt = getopt_long(argc, argv, "jvqh", opts, NULL)) != -1) {
		switch (opt) {
		case 'j':
			json = 1;
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

	struct devices *d = devices_open();
	size_t count = devices_count(d);

	if (json) {
		printf("[\n");
		for (size_t i = 0; i < count; i++) {
			const struct device_entry *e = devices_at(d, i);

			printf("%s  {\"device\": ", i ? "," : "");
			print_json_string(e->name);
			printf(", \"addr\": ");
			print_json_string(e->mac);
			printf(", \"module\": ");
			print_json_string(e->module);
			printf(", \"last_seen\": %lld}\n", e->last_seen);
		}
		printf("]\n");
	} else if (count > 0) {
		static const struct ui_col cols[] = {
			{ "DEVICE", 0 }, { "ADDRESS", 0 },
			{ "MODULE", 0 }, { "LAST SEEN", 0 },
		};
		struct ui_cell *cells = calloc(count * 4, sizeof(*cells));
		char (*when)[32] = calloc(count, sizeof(*when));

		if (cells && when) {
			for (size_t i = 0; i < count; i++) {
				const struct device_entry *e = devices_at(d, i);
				struct ui_cell *row = &cells[i * 4];

				snprintf(when[i], sizeof(when[i]), "-");
				if (e->last_seen) {
					time_t t = (time_t)e->last_seen;
					struct tm tm;

					if (localtime_r(&t, &tm))
						strftime(when[i], sizeof(when[i]),
							 "%Y-%m-%d %H:%M", &tm);
				}
				row[0].text = e->name;
				row[0].sgr = ui_c("1;36");
				row[1].text = e->mac;
				row[1].sgr = UI_DIM;
				row[2].text = e->module[0] ? e->module : "-";
				row[3].text = when[i];
				row[3].sgr = UI_DIM;
			}
			ui_table(cols, 4, cells, count);
		}
		free(cells);
		free(when);
	}

	if (!json) {
		char path[256];

		if (count == 0)
			fprintf(stderr, "%sNo module remembered yet - run "
					"`chuart scan` to find some.%s\n",
				ui_ec("2"), ui_ec("0"));
		else
			fprintf(stderr, "%sNames live in %s - edit or "
					"`chuart rename <name> <new>`%s\n",
				ui_ec("2"),
				ui_short_home(devices_path(), path, sizeof(path)),
				ui_ec("0"));
	}
	devices_close(d);
	return 0;
}

static int cmd_rename(int argc, char **argv)
{
	static const struct option opts[] = {
		{ "verbose", no_argument, NULL, 'v' },
		{ "quiet", no_argument, NULL, 'q' },
		{ "help", no_argument, NULL, 'h' },
		{ NULL, 0, NULL, 0 },
	};
	int opt;

	optind = 1;
	while ((opt = getopt_long(argc, argv, "vqh", opts, NULL)) != -1) {
		switch (opt) {
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

	if (argc - optind != 2) {
		log_error("usage: chuart rename <name|mac> <new-name>");
		return 2;
	}

	struct devices *d = devices_open();
	int rc = devices_rename(d, argv[optind], argv[optind + 1]);

	if (rc == 0) {
		if (devices_save(d) != 0)
			log_error("cannot save %s", devices_path());
		else
			printf("%s is now called '%s'\n", argv[optind],
			       argv[optind + 1]);
	}
	devices_close(d);
	return rc == 0 ? 0 : 1;
}

/* ------------------------------------------------------------------- serve --- */

/*
 * The device node is created before the module is connected, so a client can
 * set its port parameters first.  Those requests are remembered and applied as
 * soon as the link is up; afterwards they are forwarded straight away.
 */
struct serve_ctx {
	pthread_mutex_t lock;
	struct ble_link *link;
	struct tty_params desired;
	int have_desired;
};

static int on_apply_params(void *user, const struct tty_params *params)
{
	struct serve_ctx *ctx = user;
	struct ble_link *link;

	pthread_mutex_lock(&ctx->lock);
	link = ctx->link;
	if (!link) {
		ctx->desired = *params;
		ctx->have_desired = 1;
		pthread_mutex_unlock(&ctx->lock);
		log_info("client parameters noted, will apply them once the "
			 "module is connected");
		return 0;
	}
	pthread_mutex_unlock(&ctx->lock);

	return ble_link_apply_params(link, params);
}

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
		const char *device;
		const char *mac;
		const char *name;
		const char *notify_uuid;
		const char *owner;
		int mtu;
		int connect_timeout;
		int scan_time;
		mode_t mode;
	} o = {
		.name = DEFAULT_DEVICE_NAME,
		.notify_uuid = DEFAULT_NOTIFY_UUID,
		.connect_timeout = 15000,
		.scan_time = 5,
		.mode = 0,
	};
	int opt;

	optind = 1;
	while ((opt = getopt_long(argc, argv, "d:m:n:u:M:T:Y:o:O:vqh",
				  opts, NULL)) != -1) {
		switch (opt) {
		case 'd':
			o.device = optarg;
			break;
		case 'm':
			o.mac = optarg;
			break;
		case 'n':
			o.name = optarg;
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

	char mac_buf[DEVICE_MAC_MAX];
	char device_name[DEVICE_NAME_MAX];

	if (resolve_device(o.device, o.mac, mac_buf, sizeof(mac_buf), device_name,
			   sizeof(device_name)) != 0)
		return 2;
	o.mac = mac_buf;
	if (device_name[0])
		log_info("device '%s' (%s)", device_name, mac_buf);
	else
		log_info("device %s (no name assigned)", mac_buf);

	{
		struct ble_adapter_status ad;
		char why[256];

		if (ble_adapter_check(&ad, why, sizeof(why)) != 0) {
			log_error("bluetooth unusable: %s", why);
			return 1;
		}
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

	struct serve_ctx ctx = {
		.lock = PTHREAD_MUTEX_INITIALIZER,
	};
	struct cuse_tty_opts topts = {
		.name = o.name,
		.owner = o.owner,
		.mode = o.mode,
		.rx_capacity = RX_RING_BYTES,
		.tx_capacity = TX_RING_BYTES,
		.apply_params = on_apply_params,
		.apply_params_user = &ctx,
	};

	struct cuse_tty *tty = NULL;

	if (cuse_tty_start(&topts, &tty) != 0)
		return 1;

	struct ble_link_opts lopts = {
		.notify_uuid = o.notify_uuid,
		.connect_timeout_ms = o.connect_timeout,
		.pre_scan_seconds = o.scan_time,
		.mtu_cap = o.mtu,
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

	if (ble_link_startup(link) != 0) {
		ble_link_close(link);
		cuse_tty_stop(tty);
		return 1;
	}
	/* From here on client parameter changes go straight to the module. */
	pthread_mutex_lock(&ctx.lock);
	ctx.link = link;
	int have_desired = ctx.have_desired;
	struct tty_params desired = ctx.desired;
	pthread_mutex_unlock(&ctx.lock);

	if (have_desired) {
		char buf[64];

		tty_params_format(&desired, buf, sizeof(buf));
		log_info("applying the parameters the client asked for earlier: %s",
			 buf);
		if (ble_link_apply_params(link, &desired) != 0)
			log_warn("could not apply the client's earlier parameters");
	}

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

	log_info("device ready: %s (MTU %u)", cuse_tty_path(tty),
		 ble_link_mtu(link));
	printf("%sdevice ready:%s %s%s%s\n", UI_DIM, UI_RESET, ui_c("1;36"),
	       cuse_tty_path(tty), UI_RESET);
	printf("  %stry:%s picocom --nolock -b 115200 %s\n", UI_DIM, UI_RESET,
	       cuse_tty_path(tty));
	printf("  %spress Ctrl-C to stop%s\n", UI_DIM, UI_RESET);
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
	struct ble_adapter_status ad;
	char why[256];
	int rc = ble_adapter_check(&ad, why, sizeof(why));

	printf("%schuart%s      %s\n", UI_DIM, UI_RESET, CHUART_VERSION);
	printf("%sCH9140lib%s   libCH9140lib.so\n", UI_DIM, UI_RESET);
	if (ad.adapter[0])
		printf("%sadapter%s     %s (%s, %s%s%s)\n", UI_DIM, UI_RESET,
		       ad.adapter, ad.version_name,
		       ad.powered ? UI_GREEN : UI_YELLOW,
		       ad.powered ? "powered on" : "powered off", UI_RESET);
	else
		printf("%sadapter%s     %snone found%s\n", UI_DIM, UI_RESET,
		       UI_YELLOW, UI_RESET);
	if (rc != 0)
		printf("%sbluetooth%s   %sunusable: %s%s\n", UI_DIM, UI_RESET,
		       UI_RED, why, UI_RESET);
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
	if (strcmp(cmd, "devices") == 0)
		return cmd_devices(sub_argc, sub_argv);
	if (strcmp(cmd, "rename") == 0)
		return cmd_rename(sub_argc, sub_argv);
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
