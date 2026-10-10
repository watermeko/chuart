#define _GNU_SOURCE
#include "ring.h"

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>

struct ring {
	pthread_mutex_t lock;
	pthread_cond_t not_empty;
	pthread_cond_t not_full;
	unsigned char *buf;
	size_t capacity;
	size_t head;
	size_t count;
	int closed;
};

ring_t *ring_new(size_t capacity)
{
	ring_t *r;

	if (capacity == 0)
		return NULL;

	r = calloc(1, sizeof(*r));
	if (!r)
		return NULL;
	r->buf = malloc(capacity);
	if (!r->buf) {
		free(r);
		return NULL;
	}
	pthread_mutex_init(&r->lock, NULL);
	pthread_cond_init(&r->not_empty, NULL);
	pthread_cond_init(&r->not_full, NULL);
	r->capacity = capacity;
	return r;
}

void ring_free(ring_t *r)
{
	if (!r)
		return;
	pthread_mutex_destroy(&r->lock);
	pthread_cond_destroy(&r->not_empty);
	pthread_cond_destroy(&r->not_full);
	free(r->buf);
	free(r);
}

/* Absolute deadline for a timeout that starts now.  A single deadline is
 * computed per call so that repeated waits honour the total timeout. */
static void deadline_from_ms(struct timespec *ts, int timeout_ms)
{
	struct timeval now;

	gettimeofday(&now, NULL);
	ts->tv_sec = now.tv_sec + timeout_ms / 1000;
	ts->tv_nsec = (long)now.tv_usec * 1000 +
		      (long)(timeout_ms % 1000) * 1000000L;
	if (ts->tv_nsec >= 1000000000L) {
		ts->tv_sec++;
		ts->tv_nsec -= 1000000000L;
	}
}

/* Copy from src into the ring; the lock must be held. */
static size_t store_locked(ring_t *r, const unsigned char *src, size_t len)
{
	size_t written = 0;

	if (len > r->capacity - r->count)
		len = r->capacity - r->count;
	while (written < len) {
		size_t idx = (r->head + r->count) % r->capacity;
		size_t chunk = r->capacity - idx;

		if (chunk > len - written)
			chunk = len - written;
		memcpy(r->buf + idx, src + written, chunk);
		r->count += chunk;
		written += chunk;
	}
	return written;
}

/* Copy out of the ring into dst; the lock must be held. */
static size_t take_locked(ring_t *r, unsigned char *dst, size_t len)
{
	size_t taken = 0;

	if (len > r->count)
		len = r->count;
	while (taken < len) {
		size_t chunk = r->capacity - r->head;

		if (chunk > len - taken)
			chunk = len - taken;
		memcpy(dst + taken, r->buf + r->head, chunk);
		r->head = (r->head + chunk) % r->capacity;
		r->count -= chunk;
		taken += chunk;
	}
	return taken;
}

size_t ring_write(ring_t *r, const void *buf, size_t len, int timeout_ms)
{
	const unsigned char *src = buf;
	struct timespec deadline;
	size_t written = 0;

	if (!r || len == 0)
		return 0;
	if (timeout_ms > 0)
		deadline_from_ms(&deadline, timeout_ms);

	pthread_mutex_lock(&r->lock);
	while (written < len && !r->closed) {
		written += store_locked(r, src + written, len - written);
		if (written == len)
			break;
		if (timeout_ms == 0)
			break;
		if (timeout_ms > 0) {
			if (pthread_cond_timedwait(&r->not_full, &r->lock,
						   &deadline) == ETIMEDOUT)
				break;
		} else {
			pthread_cond_wait(&r->not_full, &r->lock);
		}
	}
	if (written > 0)
		pthread_cond_broadcast(&r->not_empty);
	pthread_mutex_unlock(&r->lock);
	return written;
}

ssize_t ring_read(ring_t *r, void *buf, size_t len, int timeout_ms)
{
	struct timespec deadline;
	size_t taken = 0;
	int closed;

	if (!r || len == 0)
		return 0;
	if (timeout_ms > 0)
		deadline_from_ms(&deadline, timeout_ms);

	pthread_mutex_lock(&r->lock);
	while (r->count == 0 && !r->closed) {
		if (timeout_ms == 0)
			break;
		if (timeout_ms > 0) {
			if (pthread_cond_timedwait(&r->not_empty, &r->lock,
						   &deadline) == ETIMEDOUT)
				break;
		} else {
			pthread_cond_wait(&r->not_empty, &r->lock);
		}
	}
	if (r->count > 0)
		taken = take_locked(r, buf, len);
	closed = r->closed;
	if (taken > 0)
		pthread_cond_broadcast(&r->not_full);
	pthread_mutex_unlock(&r->lock);

	if (taken == 0 && closed)
		return -1;
	return (ssize_t)taken;
}

size_t ring_count(ring_t *r)
{
	size_t n;

	if (!r)
		return 0;
	pthread_mutex_lock(&r->lock);
	n = r->count;
	pthread_mutex_unlock(&r->lock);
	return n;
}

size_t ring_capacity(ring_t *r)
{
	return r ? r->capacity : 0;
}

int ring_is_closed(ring_t *r)
{
	int closed;

	if (!r)
		return 1;
	pthread_mutex_lock(&r->lock);
	closed = r->closed;
	pthread_mutex_unlock(&r->lock);
	return closed;
}

void ring_clear(ring_t *r)
{
	if (!r)
		return;
	pthread_mutex_lock(&r->lock);
	r->head = 0;
	r->count = 0;
	pthread_cond_broadcast(&r->not_full);
	pthread_mutex_unlock(&r->lock);
}

void ring_close(ring_t *r)
{
	if (!r)
		return;
	pthread_mutex_lock(&r->lock);
	r->closed = 1;
	pthread_cond_broadcast(&r->not_empty);
	pthread_cond_broadcast(&r->not_full);
	pthread_mutex_unlock(&r->lock);
}
