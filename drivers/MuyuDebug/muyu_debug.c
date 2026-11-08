/*
 * muyu: console mirror for the USB debug port (u128).
 * Ported from ios-vermeer-lab/xnu/vermeer/vermeer_debug.c with the GUI sampler removed.
 *
 * Every console consumer (BSD printf via _cnputs, PE kprintf via serial_putc_crlf, and the
 * deferred console ring's flush) appends here, in whatever context it happens to be.  The
 * acquisition is an atomic compare-and-swap: if somebody already holds the mirror, the text is
 * counted as dropped and thrown away.  Nothing in this file waits, allocates or writes storage,
 * so it cannot contribute to the console deadlock it exists to observe.
 */
#include <sys/types.h>
#include <sys/systm.h>
#include <machine/atomic.h>

#include "muyu_log_ring.h"

static struct ml_ring log_ring;
static uint32_t _Atomic log_lock;
static uint64_t _Atomic dropped;

void muyu_debug_append(const char *data, int size);
unsigned muyu_debug_copy(uint64_t *pos, char *out, unsigned max);
uint64_t muyu_debug_total(void);
uint64_t muyu_debug_dropped(void);

void
muyu_debug_append(const char *data, int size)
{
	if (size <= 0) {
		return;
	}
	if (!os_atomic_cmpxchg(&log_lock, 0, 1, acquire)) {
		os_atomic_add(&dropped, (uint64_t)size, relaxed);
		return;
	}
	ml_append(&log_ring, data, (unsigned)size);
	os_atomic_store(&log_lock, 0, release);
}

unsigned
muyu_debug_copy(uint64_t *pos, char *out, unsigned max)
{
	if (!os_atomic_cmpxchg(&log_lock, 0, 1, acquire)) {
		return 0;
	}
	unsigned n = ml_copy(&log_ring, pos, out, max);

	os_atomic_store(&log_lock, 0, release);
	return n;
}

uint64_t
muyu_debug_total(void)
{
	if (!os_atomic_cmpxchg(&log_lock, 0, 1, acquire)) {
		return 0;
	}
	uint64_t n = log_ring.total;

	os_atomic_store(&log_lock, 0, release);
	return n;
}

uint64_t
muyu_debug_dropped(void)
{
	return os_atomic_load(&dropped, relaxed);
}
