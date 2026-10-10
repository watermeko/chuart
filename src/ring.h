#ifndef CHUART_RING_H
#define CHUART_RING_H

#include <stddef.h>
#include <sys/types.h>

/*
 * Bounded byte FIFO guarded by a mutex with two condition variables.
 *
 * Producers (the BLE notification callback and the CUSE write handler) and
 * consumers (the CUSE read handler and the BLE writer thread) run on different
 * threads.  Closing a ring unblocks every waiter, which is how link loss is
 * propagated to both directions.
 */
typedef struct ring ring_t;

ring_t *ring_new(size_t capacity);
void ring_free(ring_t *r);

/* Write as many bytes as fit, waiting for space for up to timeout_ms
 * (0 = never wait, < 0 = wait indefinitely).  Returns the number of bytes
 * stored; a short count means the timeout expired or the ring was closed. */
size_t ring_write(ring_t *r, const void *buf, size_t len, int timeout_ms);

/* Read up to len bytes, waiting for data for up to timeout_ms (0 = poll,
 * < 0 = wait indefinitely).  Returns bytes read, 0 when nothing arrived in
 * time, and -1 when the ring is closed. */
ssize_t ring_read(ring_t *r, void *buf, size_t len, int timeout_ms);

size_t ring_count(ring_t *r);
size_t ring_capacity(ring_t *r);
int ring_is_closed(ring_t *r);

/* Discard buffered data. */
void ring_clear(ring_t *r);

/* Unblock all waiters; further operations fail. */
void ring_close(ring_t *r);

#endif /* CHUART_RING_H */
