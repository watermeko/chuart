#ifndef CHUART_DEVICES_H
#define CHUART_DEVICES_H

#include <stddef.h>

#define DEVICE_NAME_MAX 24
#define DEVICE_MAC_MAX 18
#define DEVICE_MODULE_MAX 64

/*
 * A small persistent registry that gives every module a memorable name, so that
 * `chuart serve -d excitedApple` works instead of typing a MAC address.
 *
 * Names are assigned on first sight as <adjective><Fruit> out of a fixed 10x10
 * table (100 combinations) and can be changed by the user with `chuart rename`.
 * The file is plain text and may also be edited by hand.  Everything happens in
 * memory; devices_save() writes atomically.
 */
struct device_entry {
	char name[DEVICE_NAME_MAX];
	char mac[DEVICE_MAC_MAX];
	char module[DEVICE_MODULE_MAX];
	long long last_seen;
};

struct devices;

/* Where the registry lives (also useful in messages and --help). */
const char *devices_path(void);

/* Load the registry.  Never returns NULL: a missing or unreadable file simply
 * yields an empty registry. */
struct devices *devices_open(void);
void devices_close(struct devices *d);

/* Write the registry back (atomic rename).  Returns 0 or a negative errno. */
int devices_save(struct devices *d);

size_t devices_count(struct devices *d);
const struct device_entry *devices_at(struct devices *d, size_t index);

/* Find a device by friendly name or MAC address.  Returns 0 and fills
 * mac_out when it is known, -ENOENT otherwise. */
int devices_lookup(struct devices *d, const char *name_or_mac,
		   char mac_out[DEVICE_MAC_MAX]);

/* Find by MAC, adding it when unknown.  Always fills name_out on success and
 * sets *created when a fresh name was assigned.  Marks the registry dirty when
 * anything changed. */
int devices_touch(struct devices *d, const char *mac, const char *module_name,
		  char name_out[DEVICE_NAME_MAX], int *created);

/* Rename by friendly name or MAC.  Validates the new name (no spaces, at most
 * DEVICE_NAME_MAX-1 characters, not already taken).  Marks the registry dirty. */
int devices_rename(struct devices *d, const char *name_or_mac,
		   const char *new_name);

#endif /* CHUART_DEVICES_H */
