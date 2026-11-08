/*
 * muyu: lock-free-ish console ring shared by the console paths and the panel readout.
 * Ported from ios-vermeer-lab/xnu/vermeer/vermeer_log_ring.h (u128).
 *
 * The caller supplies a non-blocking acquisition; nothing here allocates, waits or touches
 * storage.  One MiB of the tail is what the host eventually reads back.
 */
#ifndef MUYU_LOG_RING_H
#define MUYU_LOG_RING_H

#include <sys/types.h>

#ifndef ML_RING_SIZE
/*
 * u128: 1 MiB (vermeer's size) does not fit this port's prelink layout - a static array that big
 * pushes __DATA over its retained segment and emit_prelink refuses to emit the image.  128 KiB is
 * ~1200 console lines, which is more than a panel photo ever gave us.
 */
#define ML_RING_SIZE (128u << 10)
#endif

struct ml_ring {
	uint64_t total;
	char bytes[ML_RING_SIZE];
};

static inline void
ml_append(struct ml_ring *r, const char *s, unsigned n)
{
	uint64_t at = r->total;

	r->total += n;
	if (n > ML_RING_SIZE) {
		unsigned skip = n - ML_RING_SIZE;
		s += skip;
		at += skip;
		n = ML_RING_SIZE;
	}
	for (unsigned i = 0; i < n; i++) {
		r->bytes[(at + i) % ML_RING_SIZE] = s[i];
	}
}

static inline unsigned
ml_copy(const struct ml_ring *r, uint64_t *pos, char *out, unsigned max)
{
	if (*pos > r->total) {
		*pos = 0;
	}
	if (r->total - *pos > ML_RING_SIZE) {
		*pos = r->total - ML_RING_SIZE;
	}
	uint64_t left = r->total - *pos;
	unsigned n = left < max ? (unsigned)left : max;

	for (unsigned i = 0; i < n; i++) {
		out[i] = r->bytes[(*pos + i) % ML_RING_SIZE];
	}
	*pos += n;
	return n;
}

#endif /* MUYU_LOG_RING_H */
