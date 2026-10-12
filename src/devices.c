#define _GNU_SOURCE
#include "devices.h"

#include "log.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

/* Short on purpose: 10 adjectives x 10 fruits = 100 names, all <= 12 characters
 * (<adjective> + <Fruit>), e.g. excitedApple. */
static const char *const adjectives[] = {
	"bold", "brave", "calm", "eager", "excited",
	"happy", "lucky", "quick", "sharp", "sunny",
};

static const char *const fruits[] = {
	"apple", "fig", "kiwi", "lemon", "lime",
	"mango", "melon", "olive", "peach", "plum",
};

#define N_ADJECTIVES (sizeof(adjectives) / sizeof(adjectives[0]))
#define N_FRUITS (sizeof(fruits) / sizeof(fruits[0]))

struct devices {
	struct device_entry *entries;
	size_t count;
	size_t capacity;
	int dirty;
};

/* ------------------------------------------------------------------- path --- */

/*
 * The registry belongs to the human who runs chuart, but `serve` needs root for
 * /dev/cuse.  When invoked through sudo we therefore use the invoking user's
 * home rather than /root, so that names assigned by `chuart scan` are visible to
 * `sudo chuart serve -d <name>` and vice versa.
 */
const char *devices_path(void)
{
	static char path[4096];

	if (path[0])
		return path;

	const char *dir = getenv("CHUART_CONFIG_DIR");
	if (dir && *dir) {
		snprintf(path, sizeof(path), "%s/devices.conf", dir);
		return path;
	}

	const char *config_home = getenv("XDG_CONFIG_HOME");
	if (config_home && *config_home) {
		snprintf(path, sizeof(path), "%s/chuart/devices.conf", config_home);
		return path;
	}

	const char *home = NULL;
	const char *sudo_user = getenv("SUDO_USER");

	if (sudo_user && *sudo_user && geteuid() == 0) {
		struct passwd *pw = getpwnam(sudo_user);

		if (pw && pw->pw_dir && *pw->pw_dir)
			home = pw->pw_dir;
	}
	if (!home)
		home = getenv("HOME");
	if (!home || !*home) {
		struct passwd *pw = getpwuid(getuid());

		home = (pw && pw->pw_dir && *pw->pw_dir) ? pw->pw_dir : "/tmp";
	}

	snprintf(path, sizeof(path), "%s/.config/chuart/devices.conf", home);
	return path;
}

/* Ownership of the invoking user, so a sudo run does not leave files only root
 * can edit. */
static void invoking_owner(uid_t *uid, gid_t *gid)
{
	const char *sudo_user = getenv("SUDO_USER");

	*uid = (uid_t)-1;
	*gid = (gid_t)-1;
	if (sudo_user && *sudo_user && geteuid() == 0) {
		struct passwd *pw = getpwnam(sudo_user);

		if (pw) {
			*uid = pw->pw_uid;
			*gid = pw->pw_gid;
		}
	}
}

/* Create the parent directory, giving it to the invoking user when possible. */
static int ensure_parent_dir(const char *path)
{
	char dir[4096];
	char *slash;
	uid_t uid;
	gid_t gid;

	snprintf(dir, sizeof(dir), "%s", path);
	slash = strrchr(dir, '/');
	if (!slash || slash == dir)
		return 0;
	*slash = '\0';

	if (mkdir(dir, 0700) != 0 && errno != EEXIST)
		return -errno;

	invoking_owner(&uid, &gid);
	if (uid != (uid_t)-1 && chown(dir, uid, gid) != 0)
		log_debug("chown(%s) failed: %s", dir, strerror(errno));
	return 0;
}

/* ------------------------------------------------------------------ random --- */

static void seed_random(void)
{
	static int seeded;
	unsigned int seed = 0;
	int fd;

	if (seeded)
		return;

	fd = open("/dev/urandom", O_RDONLY);
	if (fd >= 0) {
		if (read(fd, &seed, sizeof(seed)) != (ssize_t)sizeof(seed))
			seed = 0;
		close(fd);
	}
	if (seed == 0)
		seed = (unsigned int)time(NULL) ^ (unsigned int)getpid();
	srandom(seed);
	seeded = 1;
}

/* ------------------------------------------------------------------ lookup --- */

static int mac_equal(const char *a, const char *b)
{
	return strcasecmp(a, b) == 0;
}

static int name_equal(const char *a, const char *b)
{
	return strcasecmp(a, b) == 0;
}

/* "50:54:7b:..." -> "50:54:7B:..." (upper case, colons as separators). */
static void normalize_mac(const char *in, char out[DEVICE_MAC_MAX])
{
	size_t j = 0;

	for (const char *p = in; *p && j + 1 < DEVICE_MAC_MAX; p++)
		out[j++] = (char)toupper((unsigned char)*p);
	out[j] = '\0';
}

static int looks_like_mac(const char *s)
{
	int colons = 0;
	int digits = 0;

	for (const char *p = s; *p; p++) {
		if (*p == ':')
			colons++;
		else if (isxdigit((unsigned char)*p))
			digits++;
		else
			return 0;
	}
	return colons == 5 && digits == 12;
}

static struct device_entry *find_by_mac(struct devices *d, const char *mac)
{
	for (size_t i = 0; i < d->count; i++)
		if (mac_equal(d->entries[i].mac, mac))
			return &d->entries[i];
	return NULL;
}

static struct device_entry *find_by_name(struct devices *d, const char *name)
{
	for (size_t i = 0; i < d->count; i++)
		if (name_equal(d->entries[i].name, name))
			return &d->entries[i];
	return NULL;
}

/* --------------------------------------------------------------- registry --- */

struct devices *devices_open(void)
{
	struct devices *d = calloc(1, sizeof(*d));
	FILE *f;
	char line[512];

	if (!d)
		return NULL;

	f = fopen(devices_path(), "r");
	if (!f)
		return d;

	while (fgets(line, sizeof(line), f)) {
		char *p = line;
		char *name;
		char *mac;
		char *module;
		long long last_seen = 0;

		while (*p == ' ' || *p == '\t')
			p++;
		if (*p == '#' || *p == '\n' || *p == '\0')
			continue;

		name = p;
		while (*p && *p != ' ' && *p != '\t')
			p++;
		if (!*p)
			continue;
		*p++ = '\0';

		while (*p == ' ' || *p == '\t')
			p++;
		mac = p;
		while (*p && *p != ' ' && *p != '\t')
			p++;
		if (!*p)
			*p = '\0'; /* last field on the line */
		else
			*p++ = '\0';
		if (!looks_like_mac(mac))
			continue;

		while (*p == ' ' || *p == '\t')
			p++;
		/* Optional epoch seconds; anything else is part of the module name
		 * (module names may contain spaces). */
		if (isdigit((unsigned char)*p)) {
			char *end = NULL;

			last_seen = strtoll(p, &end, 10);
			if (end && end != p) {
				p = end;
				while (*p == ' ' || *p == '\t')
					p++;
			} else {
				last_seen = 0;
			}
		}
		module = p;
		char *eol = module + strlen(module);

		while (eol > module && (eol[-1] == '\n' || eol[-1] == '\r' ||
					eol[-1] == ' ' || eol[-1] == '\t'))
			*--eol = '\0';
		if (!*module)
			module = NULL;

		if (d->count == d->capacity) {
			size_t cap = d->capacity ? d->capacity * 2 : 8;
			struct device_entry *grown =
				realloc(d->entries, cap * sizeof(*grown));

			if (!grown)
				break;
			d->entries = grown;
			d->capacity = cap;
		}
		struct device_entry *entry = &d->entries[d->count++];

		memset(entry, 0, sizeof(*entry));
		snprintf(entry->name, sizeof(entry->name), "%.*s",
			 (int)sizeof(entry->name) - 1, name);
		normalize_mac(mac, entry->mac);
		entry->last_seen = last_seen;
		if (module)
			snprintf(entry->module, sizeof(entry->module), "%.*s",
				 (int)sizeof(entry->module) - 1, module);
	}
	fclose(f);
	return d;
}

void devices_close(struct devices *d)
{
	if (!d)
		return;
	free(d->entries);
	free(d);
}

size_t devices_count(struct devices *d)
{
	return d ? d->count : 0;
}

const struct device_entry *devices_at(struct devices *d, size_t index)
{
	if (!d || index >= d->count)
		return NULL;
	return &d->entries[index];
}

int devices_save(struct devices *d)
{
	char tmp[4096];
	const char *path = devices_path();
	FILE *f;
	int fd;
	uid_t uid;
	gid_t gid;
	int rc;

	if (!d || !d->dirty)
		return 0;
	if (ensure_parent_dir(path) != 0)
		return -errno;

	snprintf(tmp, sizeof(tmp), "%s.tmp", path);
	fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
	if (fd < 0)
		return -errno;
	f = fdopen(fd, "w");
	if (!f) {
		rc = -errno;
		close(fd);
		unlink(tmp);
		return rc;
	}

	fprintf(f, "# chuart devices: <name> <mac> [module name]\n");
	fprintf(f, "# Edit by hand if you like; `chuart devices` lists them,\n");
	fprintf(f, "# `chuart rename <name|mac> <new-name>` renames one.\n");
	for (size_t i = 0; i < d->count; i++)
		fprintf(f, "%s %s %lld%s%s\n", d->entries[i].name,
			d->entries[i].mac, d->entries[i].last_seen,
			d->entries[i].module[0] ? " " : "",
			d->entries[i].module);

	if (fflush(f) != 0 || ferror(f)) {
		rc = -EIO;
		fclose(f);
		unlink(tmp);
		return rc;
	}

	invoking_owner(&uid, &gid);
	if (uid != (uid_t)-1 && fchown(fd, uid, gid) != 0)
		log_debug("fchown(%s) failed: %s", tmp, strerror(errno));
	fclose(f);

	if (rename(tmp, path) != 0) {
		rc = -errno;
		unlink(tmp);
		return rc;
	}
	d->dirty = 0;
	return 0;
}

/* ------------------------------------------------------------------- names --- */

/* Pick a free adjective+fruit combination: the 100 candidates are marked, then
 * one of the free ones is drawn at random. */
static int assign_name(struct devices *d, char out[DEVICE_NAME_MAX])
{
	enum { TOTAL = N_ADJECTIVES * N_FRUITS };
	unsigned char used[TOTAL] = { 0 };
	size_t free_count = TOTAL;

	for (size_t i = 0; i < d->count; i++) {
		for (size_t a = 0; a < N_ADJECTIVES; a++) {
			for (size_t f = 0; f < N_FRUITS; f++) {
				char candidate[DEVICE_NAME_MAX];
				char fruit[16];

				snprintf(fruit, sizeof(fruit), "%s", fruits[f]);
				fruit[0] = (char)toupper((unsigned char)fruit[0]);
				snprintf(candidate, sizeof(candidate), "%s%s",
					 adjectives[a], fruit);
				if (name_equal(candidate, d->entries[i].name) &&
				    !used[a * N_FRUITS + f]) {
					used[a * N_FRUITS + f] = 1;
					free_count--;
				}
			}
		}
	}
	if (free_count == 0)
		return -ENOSPC;

	seed_random();
	size_t pick = (size_t)(random() % (long)free_count);

	for (size_t i = 0, n = 0; i < TOTAL; i++) {
		if (used[i])
			continue;
		if (n++ == pick) {
			char fruit[16];

			/* camelCase, as in "excitedApple" */
			snprintf(fruit, sizeof(fruit), "%s", fruits[i % N_FRUITS]);
			fruit[0] = (char)toupper((unsigned char)fruit[0]);
			snprintf(out, DEVICE_NAME_MAX, "%s%s",
				 adjectives[i / N_FRUITS], fruit);
			return 0;
		}
	}
	return -ENOSPC;
}

static int add_entry(struct devices *d, const char *mac, const char *module,
		     const char *name)
{
	if (d->count == d->capacity) {
		size_t cap = d->capacity ? d->capacity * 2 : 8;
		struct device_entry *grown = realloc(d->entries, cap * sizeof(*grown));

		if (!grown)
			return -ENOMEM;
		d->entries = grown;
		d->capacity = cap;
	}
	struct device_entry *e = &d->entries[d->count++];

	memset(e, 0, sizeof(*e));
	snprintf(e->name, sizeof(e->name), "%s", name);
	normalize_mac(mac, e->mac);
	if (module)
		snprintf(e->module, sizeof(e->module), "%s", module);
	e->last_seen = (long long)time(NULL);
	return 0;
}

int devices_touch(struct devices *d, const char *mac, const char *module_name,
		  char name_out[DEVICE_NAME_MAX], int *created)
{
	struct device_entry *e;
	char normalized[DEVICE_MAC_MAX];

	if (created)
		*created = 0;
	if (!d || !mac || !looks_like_mac(mac))
		return -EINVAL;

	normalize_mac(mac, normalized);
	e = find_by_mac(d, normalized);
	if (e) {
		if (module_name && *module_name &&
		    strcmp(e->module, module_name) != 0) {
			snprintf(e->module, sizeof(e->module), "%s", module_name);
			d->dirty = 1;
		}
		e->last_seen = (long long)time(NULL);
		if (name_out)
			snprintf(name_out, DEVICE_NAME_MAX, "%s", e->name);
		return 0;
	}

	char name[DEVICE_NAME_MAX];
	int rc = assign_name(d, name);

	if (rc != 0)
		return rc; /* callers report it; it is not fatal for them */
	rc = add_entry(d, normalized, module_name, name);
	if (rc != 0)
		return rc;
	d->dirty = 1;
	if (created)
		*created = 1;
	if (name_out)
		snprintf(name_out, DEVICE_NAME_MAX, "%s", name);
	return 0;
}

/* ------------------------------------------------------------------ lookup --- */

int devices_lookup(struct devices *d, const char *name_or_mac,
		   char mac_out[DEVICE_MAC_MAX])
{
	if (!d || !name_or_mac || !*name_or_mac)
		return -EINVAL;

	if (looks_like_mac(name_or_mac)) {
		normalize_mac(name_or_mac, mac_out);
		return 0;
	}

	struct device_entry *e = find_by_name(d, name_or_mac);

	if (!e)
		return -ENOENT;
	snprintf(mac_out, DEVICE_MAC_MAX, "%s", e->mac);
	return 0;
}

/* --------------------------------------------------------------- renaming --- */

static int valid_name(const char *name)
{
	size_t len;

	if (!name || !(len = strlen(name)) || len >= DEVICE_NAME_MAX)
		return 0;
	if (looks_like_mac(name))
		return 0;
	for (const char *p = name; *p; p++)
		if (!isalnum((unsigned char)*p) && *p != '-' && *p != '_')
			return 0;
	return 1;
}

int devices_rename(struct devices *d, const char *name_or_mac,
		   const char *new_name)
{
	struct device_entry *e;

	if (!d || !name_or_mac || !*name_or_mac)
		return -EINVAL;
	if (!valid_name(new_name)) {
		log_error("invalid device name '%s' (letters, digits, '-' and '_' "
			  "only, 1-%d characters)", new_name ? new_name : "",
			  DEVICE_NAME_MAX - 1);
		return -EINVAL;
	}

	if (looks_like_mac(name_or_mac)) {
		char mac[DEVICE_MAC_MAX];

		normalize_mac(name_or_mac, mac);
		e = find_by_mac(d, mac);
	} else {
		e = find_by_name(d, name_or_mac);
	}
	if (!e) {
		log_error("no device named '%s' (see `chuart devices`)",
			  name_or_mac);
		return -ENOENT;
	}

	struct device_entry *clash = find_by_name(d, new_name);

	if (clash && clash != e) {
		log_error("'%s' is already used by %s", new_name, clash->mac);
		return -EEXIST;
	}
	snprintf(e->name, sizeof(e->name), "%s", new_name);
	d->dirty = 1;
	return 0;
}
