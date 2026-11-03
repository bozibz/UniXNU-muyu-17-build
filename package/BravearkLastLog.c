// SPDX-License-Identifier: MIT
// Keep a persistent dump of the kernel message buffer in the reserved pram.
// The low half keeps the embedded-panic FUNK record that the next XNU boot
// prints; the high half is a ramoops-compatible console ring
// (persistent_ram_buffer, sig 0x43474244) that a reader can expose at
// /sys/fs/pstore/console-ramoops-0. Enabled with sheng-log=1. Only writes the
// pram mapping the device tree already reserved; it never allocates, changes
// translation, or touches a device register.
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <sys/appleapiopts.h>
#include <mach/vm_types.h>
#include <kern/debug.h>
#include <kern/misc_protos.h>
#include <kern/sched.h>
#include <kern/startup.h>
#include <kern/thread.h>
#include <pexpert/pexpert.h>
#include <sys/msgbuf.h>
#include <IOKit/IOLib.h>

extern vm_offset_t gPanicBase;
extern unsigned int gPanicSize;

#define BRAVEARK_LAST_LOG_INTERVAL_MS 500

// The 4 MiB pram region is split in two halves. The low half keeps the
// embedded-panic FUNK record for the XNU console; the high half is a Linux
// ramoops console zone (persistent_ram_buffer, no ECC), which the uke Linux
// ramoops backend exposes at /sys/fs/pstore/console-ramoops-0.
#define BRAVEARK_FUNK_LOG_SIZE   0x200000u
#define BRAVEARK_RAMOOPS_OFFSET  0x200000u
#define BRAVEARK_RAMOOPS_SIZE    0x200000u
#define BRAVEARK_RAMOOPS_MAGIC   0x43474244u /* "DBGC", see Linux pstore_ram.h */

struct braveark_ramoops_buffer {
	uint32_t sig;
	uint32_t start;
	uint32_t size;
	uint8_t data[];
};

static uint32_t braveark_ramoops_last_write;
static int braveark_ramoops_initialized;

static void
braveark_ramoops_append(struct braveark_ramoops_buffer *buffer, size_t capacity,
    const uint8_t *src, size_t count)
{
	size_t start, size, first;

	if (count == 0) {
		return;
	}
	if (count > capacity) {
		src += count - capacity;
		count = capacity;
	}
	start = buffer->start;
	size = buffer->size;
	if (start >= capacity) {
		start = 0;
	}
	if (size > capacity) {
		size = capacity;
	}
	first = capacity - start;
	if (first > count) {
		first = count;
	}
	memcpy(buffer->data + start, src, first);
	if (count > first) {
		memcpy(buffer->data, src + first, count - first);
	}
	size += count;
	if (size > capacity) {
		size = capacity;
	}
	start = (start + count) % capacity;
	// The Linux reader runs after a reset, but publish the bytes before the
	// header so a concurrent reader never sees a size that covers stale data.
	__asm__ volatile("dsb sy" ::: "memory");
	buffer->size = (uint32_t)size;
	buffer->start = (uint32_t)start;
	__asm__ volatile("dsb sy" ::: "memory");
}

static void
braveark_ramoops_write(struct msgbuf *m)
{
	struct braveark_ramoops_buffer *buffer;
	size_t capacity, size, start, first, tail;
	int write;

	if (gPanicBase == 0 || gPanicSize < BRAVEARK_RAMOOPS_OFFSET + BRAVEARK_RAMOOPS_SIZE) {
		return;
	}
	buffer = (struct braveark_ramoops_buffer *)(uintptr_t)(gPanicBase + BRAVEARK_RAMOOPS_OFFSET);
	capacity = BRAVEARK_RAMOOPS_SIZE - sizeof(*buffer);
	write = m->msg_bufx;
	if (write <= 0) {
		return;
	}
	size = (size_t)m->msg_size;
	if (!braveark_ramoops_initialized) {
		buffer->sig = BRAVEARK_RAMOOPS_MAGIC;
		buffer->start = 0;
		buffer->size = 0;
		__asm__ volatile("dsb sy" ::: "memory");
		braveark_ramoops_initialized = 1;
		// Seed the ring with the current msgbuf tail: the display/root work
		// runs before the first 500 ms tick, so a delta-only start would lose
		// the interesting beginning of the boot.
		tail = capacity;
		if (tail > size) {
			tail = size;
		}
		if (tail > (size_t)write) {
			tail = (size_t)write;
		}
		start = ((size_t)write + size - tail) % size;
		first = size - start;
		if (first > tail) {
			first = tail;
		}
		braveark_ramoops_append(buffer, capacity,
		    (const uint8_t *)m->msg_bufc + start, first);
		if (tail > first) {
			braveark_ramoops_append(buffer, capacity, (const uint8_t *)m->msg_bufc,
			    tail - first);
		}
		braveark_ramoops_last_write = (uint32_t)write;
		return;
	}
	if ((uint32_t)write == braveark_ramoops_last_write) {
		return;
	}
	if ((uint32_t)write > braveark_ramoops_last_write) {
		braveark_ramoops_append(buffer, capacity,
		    (const uint8_t *)m->msg_bufc + braveark_ramoops_last_write,
		    (size_t)write - braveark_ramoops_last_write);
	} else {
		braveark_ramoops_append(buffer, capacity,
		    (const uint8_t *)m->msg_bufc + braveark_ramoops_last_write,
		    size - braveark_ramoops_last_write);
		braveark_ramoops_append(buffer, capacity, (const uint8_t *)m->msg_bufc,
		    (size_t)write);
	}
	braveark_ramoops_last_write = (uint32_t)write;
}

static void
braveark_last_log_write(void)
{
	struct embedded_panic_header *hdr;
	struct msgbuf *m = msgbufp;
	size_t size, cap, start, first;
	int write;

	if (gPanicBase == 0 || gPanicSize <= sizeof(struct embedded_panic_header) + 1) {
		return;
	}
	if (m == NULL || m->msg_bufc == NULL || m->msg_size <= 1 || m->msg_magic != MSG_MAGIC) {
		return;
	}
	write = m->msg_bufx;
	if (write <= 0) {
		return;
	}

	hdr = (struct embedded_panic_header *)(uintptr_t)gPanicBase;
	size = (size_t)m->msg_size;
	cap = BRAVEARK_FUNK_LOG_SIZE - sizeof(*hdr) - 1;
	if (cap > size) {
		cap = size;
	}

	// msg_bufr only advances when a reader drains the log, so it may point at
	// stale bytes after the ring wraps. The newest cap bytes before msg_bufx
	// are the ones a reset needs.
	start = ((size_t)write + size - cap) % size;
	first = size - start;
	if (first > cap) {
		first = cap;
	}
	memcpy((void *)(hdr + 1), m->msg_bufc + start, first);
	if (cap > first) {
		memcpy((char *)(hdr + 1) + first, m->msg_bufc, cap - first);
	}
	((char *)(hdr + 1))[cap] = '\0';

	hdr->eph_magic = EMBEDDED_PANIC_MAGIC;
	hdr->eph_crc = 0;
	hdr->eph_version = EMBEDDED_PANIC_HEADER_CURRENT_VERSION;
	hdr->eph_panic_flags = 0;
	hdr->eph_panic_log_offset = (uint32_t)sizeof(*hdr);
	hdr->eph_panic_log_len = (uint32_t)cap;
	hdr->eph_stackshot_offset = 0;
	hdr->eph_stackshot_len = 0;
	hdr->eph_other_log_offset = 0;
	hdr->eph_other_log_len = 0;
	__asm__ volatile("dsb sy" ::: "memory");

	braveark_ramoops_write(m);
}

/* Filtered snapshot of the live message buffer, shared by the timed dump, the marker dump and the
 * critical-exit hook. Returns the number of bytes written to out (always NUL terminated). */
#define MUYU_FLAT_BYTES 65536
static char muyu_flat[MUYU_FLAT_BYTES];

static int
muyu_keep(const char *line, size_t len)
{
	char list[200];
	char probe[400];
	size_t copy = len < sizeof(probe) - 1 ? len : sizeof(probe) - 1;
	const char *token;

	if (!PE_parse_boot_argn("muyu-dump-keep", list, (int)sizeof(list))) {
		return -1;
	}
	memcpy(probe, line, copy);
	probe[copy] = '\0';
	for (token = list; *token;) {
		const char *end = token;
		size_t n;
		char needle[64];

		while (*end && *end != ',') {
			end++;
		}
		n = (size_t)(end - token);
		/* The token must be terminated: searching with the rest of the comma-separated list still
		 * attached never matches (that bug made every filtered dump report 0 lines). */
		if (n > 0 && n < sizeof(needle)) {
			memcpy(needle, token, n);
			needle[n] = '\0';
			if (strnstr(probe, needle, copy) != NULL) {
				return 1;
			}
		}
		token = *end ? end + 1 : end;
	}
	return 0;
}

static int
muyu_noise(const char *line, size_t len)
{
	static const char *skip[] = { "CoreAnalyticsHub", "messageClients of", "LiveActivities",
	    "For-debug:", "temporary sandbox", "deny(1)", "apfs_gpm", "APFS: disk",
	    /* 2026-11-02: with the MUYU-only filter removed, these two filled the whole 2.4 KiB
	     * window and pushed the interesting tail (the launchd exec + exit that ends the boot)
	     * out of it. Both are high-volume and say nothing about why the boot stopped. */
	    "spaceman_", "IOPlatformProcessor_register" };
	char probe[256];
	size_t copy = len < sizeof(probe) - 1 ? len : sizeof(probe) - 1;

	memcpy(probe, line, copy);
	probe[copy] = '\0';
	for (size_t i = 0; i < sizeof(skip) / sizeof(skip[0]); i++) {
		if (strnstr(probe, skip[i], copy) != NULL) {
			return 1;
		}
	}
	return 0;
}

size_t
muyu_build_dump(char *out, size_t cap)
{
	struct msgbuf *m = msgbufp;
	size_t used = 0, at, want, size, start, first;
	if (out == NULL || cap < 2) {
		return 0;
	}
	out[0] = '\0';
	if (m == NULL || m->msg_bufc == NULL || m->msg_size <= 1 || m->msg_magic != MSG_MAGIC) {
		return 0;
	}
	size = (size_t)m->msg_size;
	want = (size_t)m->msg_bufx;
	if (want == 0) {
		return 0;
	}
	want = want < MUYU_FLAT_BYTES ? want : MUYU_FLAT_BYTES;
	start = ((size_t)m->msg_bufx + size - want) % size;
	first = size - start;
	if (first > want) {
		first = want;
	}
	memcpy(muyu_flat, m->msg_bufc + start, first);
	if (want > first) {
		memcpy(muyu_flat + first, m->msg_bufc, want - first);
	}
	at = want;
	while (at > 0 && used + 1 < cap) {
		size_t line_start = at;
		size_t len;

		while (line_start > 0 && muyu_flat[line_start - 1] != '\n') {
			line_start--;
		}
		len = at - line_start;
		while (len > 0 && (muyu_flat[line_start + len - 1] == '\r' ||
		    muyu_flat[line_start + len - 1] == '\0')) {
			len--;
		}
		if (len > 0) {
			int keep = muyu_keep(muyu_flat + line_start, len);
			if (keep == 1 || (keep < 0 && !muyu_noise(muyu_flat + line_start, len))) {
				if (used + len + 1 >= cap) {
					break;
				}
				memmove(out + len + 1, out, used);
				memcpy(out, muyu_flat + line_start, len);
				out[len] = '\n';
				used += len + 1;
			}
		}
		if (line_start == 0) {
			break;
		}
		at = line_start - 1;
	}
	/* Bring-up: the display path's counters go on top of every dump. Its prints scroll out of the
	 * window within a minute, but the counters say whether the compositor was still submitting and
	 * whether the last swap retired, and the timestamps say when the path stopped. */
	{
		extern char *native_legacy_state_line(char *, unsigned) __attribute__((weak_import));
		extern void muyu_proc_census(char *, size_t) __attribute__((weak_import));
		char line[192];
		size_t length;

		if (&muyu_proc_census != NULL) {
			muyu_proc_census(line, sizeof(line));
			length = strlen(line);
			if (length > 0 && length + used < cap) {
				memmove(out + length, out, used + 1);
				memcpy(out, line, length);
				used += length;
			}
		}
		extern void muyu_usb_state_line(char *, size_t) __attribute__((weak_import));
		if (&muyu_usb_state_line != NULL) {
			muyu_usb_state_line(line, sizeof(line));
			length = strlen(line);
			if (length > 0 && length + used < cap) {
				memmove(out + length, out, used + 1);
				memcpy(out, line, length);
				used += length;
			}
		}
		if (&native_legacy_state_line != NULL) {
			native_legacy_state_line(line, (unsigned)sizeof(line));
			length = strlen(line);
			if (length > 0 && length + used < cap) {
				memmove(out + length, out, used + 1);
				memcpy(out, line, length);
				used += length;
			}
		}
	}
	out[used] = '\0';
	return used;
}

/* Snapshot text taken when the marker first appears (the buffer wraps within seconds on this board,
 * so the text must be copied then, not after the delay). */
#define MUYU_SAVED_BYTES 2401
static char muyu_saved[MUYU_SAVED_BYTES];
static size_t muyu_saved_len;

void muyu_console_wake(void);

/* muyu-dump-on=<substring> (+ muyu-dump-after=<seconds>) fires the msgbuf dump a fixed time after that
 * substring first appears in the newest part of the buffer. Timing-based dumps miss the moment: the
 * board's boot is slow, and a panic screen drawn while SpringBoard still presents is overpainted by
 * the next frame, so the dump must wait until the display client is gone. */
static int
muyu_dump_on_seen(void)
{
	char wanted[96];
	static unsigned fired = 0;
	uint32_t after = 0;
	struct msgbuf *m = msgbufp;
	char probe[512];
	size_t size, window, start, first, copy;
	int write;

	if (fired) {
		return 0;
	}
	if (!PE_parse_boot_argn("muyu-dump-on", wanted, (int)sizeof(wanted)) || wanted[0] == '\0') {
		return 0;
	}
	if (m == NULL || m->msg_bufc == NULL || m->msg_size <= 1 || m->msg_magic != MSG_MAGIC) {
		return 0;
	}
	write = m->msg_bufx;
	if (write <= 0) {
		return 0;
	}
	size = (size_t)m->msg_size;
	window = 2048;
	if (window > (size_t)write) {
		window = (size_t)write;
	}
	start = ((size_t)write + size - window) % size;
	first = size - start;
	if (first > window) {
		first = window;
	}
	copy = first < sizeof(probe) - 1 ? first : sizeof(probe) - 1;
	memcpy(probe, m->msg_bufc + start, copy);
	probe[copy] = '\0';
	if (window > first && copy < sizeof(probe) - 1) {
		size_t more = window - first;
		if (more > sizeof(probe) - 1 - copy) {
			more = sizeof(probe) - 1 - copy;
		}
		memcpy(probe + copy, m->msg_bufc, more);
		probe[copy + more] = '\0';
	}
	if (strnstr(probe, wanted, copy) == NULL) {
		return 0;
	}
	muyu_saved_len = muyu_build_dump(muyu_saved, sizeof(muyu_saved));
	if (!PE_parse_boot_argn("muyu-dump-after", &after, (int)sizeof(after))) {
		after = 0;
	}
	if (after) {
		IOSleep(after * 1000);
	}
	fired = 1;
	return 1;
}

void muyu_panic_with_log(void);
void muyu_panic_crash_report(void);

/* Bring-up: take the panel away from the display client before the dump prints. The client keeps
 * presenting frames for minutes after the boot's last useful one, so a late dump used to be painted
 * over within a frame. See native_legacy_swap_hold. */
void
muyu_console_take_panel(void)
{
	extern volatile int native_legacy_swap_hold __attribute__((weak_import));

	if (&native_legacy_swap_hold != NULL) {
		native_legacy_swap_hold = 1;
		/* Let a swap that is already past the validator finish, so its copy cannot land on the text. */
		IOSleep(150);
	}
}

static void
braveark_last_log_writer(void *arg, wait_result_t result)
{
	(void)arg;
	(void)result;

	for (;;) {
		braveark_last_log_write();
		if (muyu_dump_on_seen()) {
			unsigned int crash = 0;

			/* muyu-dump-crash=1: userland logs live in logd and in ReportCrash's files on the
			 * Data volume, not in msgbuf, so render the newest crash report instead. */
			if (PE_parse_boot_argn("muyu-dump-crash", &crash, (int)sizeof(crash)) && crash) {
				muyu_panic_crash_report();
			}
			muyu_console_take_panel();
			muyu_console_wake();
			panic("LASTLOG %u bytes, snapshot at marker (filtered):\n%s",
			    (unsigned)muyu_saved_len, muyu_saved);
		}
		IOSleep(BRAVEARK_LAST_LOG_INTERVAL_MS);
	}
}

static void
braveark_last_log_start(void)
{
	uint32_t enabled = 0;
	thread_t thread;

	if (!PE_parse_boot_argn("sheng-log", &enabled, (int)sizeof(enabled)) || enabled != 1) {
		return;
	}
	if (gPanicBase == 0 || gPanicSize <= sizeof(struct embedded_panic_header) + 1) {
		return;
	}
	if (kernel_thread_start_priority(braveark_last_log_writer, NULL, BASEPRI_KERNEL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}

STARTUP(EARLY_BOOT, STARTUP_RANK_LAST, braveark_last_log_start);


/* Bring-up: the kernel text console is parked once the render server owns the scanout (the console
 * otherwise paints over the frame buffer until the panel shows nothing but text). Every panic path
 * unparks it first, so a dump is still readable. */
void
muyu_console_wake(void)
{
	extern void vc_set_text_quiet(int) __attribute__((weak_import));

	if (&vc_set_text_quiet != NULL) {
		vc_set_text_quiet(0);
	}
}

/* Critical-exit hook: build the filtered dump and panic with it (see kern_exit.c). */
void
muyu_panic_with_log(void)
{
	static char out[MUYU_SAVED_BYTES];

	muyu_console_take_panel();
	muyu_console_wake();
	{
		/* muyu touch bring-up: run the probe here, not during the display bring-up, so a bus fault in the
		 * probe cannot cost the dump and its rows always land in the same panic text. */
		extern void native_touch_muyu_probe(void) __attribute__((weak_import));
		/* The touch frame pump (package/TouchProbeMuyu.cpp) logs a row every couple of seconds; its IOLog
		 * lines repaint the console and cover this panic screen with small text (measured 2026-10-25 21:13).
		 * Tell it to go quiet before the dump is built, so the rows it already logged survive on the panel. */
		extern volatile unsigned int native_touch_muyu_log_off __attribute__((weak_import));
		if (&native_touch_muyu_log_off != NULL) {
			native_touch_muyu_log_off = 1;
		}
		if (&native_touch_muyu_probe != NULL) {
			native_touch_muyu_probe();
		}
	}
	/* 2026-11-03: userland helpers write their failures to stderr (/dev/console); the video console
	 * only replays msgbuf, so push the console ring into the log first. It becomes the newest msgbuf
	 * lines and therefore part of the dump below - the only way to see why mount(8) exited 66. */
	{
		extern size_t muyu_console_tail(char *, size_t) __attribute__((weak_import));
		if (&muyu_console_tail != NULL) {
			static char tail[1024];
			size_t have = muyu_console_tail(tail, sizeof(tail) - 1);
			if (have > 0) {
				tail[have] = '\0';
				printf("CONSOLE TAIL (%u bytes):\n%s\n", (unsigned)have, tail);
			}
		}
	}
	muyu_build_dump(out, sizeof(out));
	panic("LASTLOG %u bytes (filtered):\n%s", (unsigned)strlen(out), out);
}

/* muyu-dump-at=<seconds>, started from load_init_program so the delay uses the calibrated clock.
 *
 * 2026-10-27 pm: this used to panic at the requested time. On this port a panic in a late boot does not
 * leave the text on the panel (the display client's frames come back, and the panic may reset), so the
 * dump was captured only by luck. It now takes the panel (native_legacy_swap_hold), unparks the text
 * console and prints the same snapshot every five seconds. Nothing paints over it, and if anything
 * still does, the next pass repaints it: the screen can be photographed at leisure, and no panic means
 * no reset and no stackshot. */
static void
muyu_dump_later(void *arg, wait_result_t result)
{
	uint32_t at = (uint32_t)(uintptr_t)arg;
	static char out[MUYU_SAVED_BYTES];
	(void)result;

	IOSleep(at * 1000);
	muyu_console_take_panel();
	muyu_console_wake();
	{
		/* Same quieting the panic path does, so the touch pump's rows cannot push the dump off. */
		extern volatile unsigned int native_touch_muyu_log_off __attribute__((weak_import));
		extern void native_touch_muyu_probe(void) __attribute__((weak_import));
		if (&native_touch_muyu_log_off != NULL) {
			native_touch_muyu_log_off = 1;
		}
		if (&native_touch_muyu_probe != NULL) {
			native_touch_muyu_probe();
		}
	}
	muyu_build_dump(out, sizeof(out));
	printf("LASTLOG %u bytes (filtered):\n%s\n", (unsigned)strlen(out), out);
	/* Bring-up measurement: after the one-shot snapshot, one short line every five seconds. This page is
	 * the instrument for the physical buttons. native_buttons_stats[] is otherwise only visible as the
	 * "MUYU BTN:" lines printed during the first seconds of the boot, which have scrolled out of the
	 * framebuffer console long before anyone can read them, so a driver that armed nothing and a driver
	 * that works both looked like "no reaction". Pressing a button must move irq/rep and t must keep
	 * climbing, so one photo of this screen is evidence either way.
	 * Rebuilding the whole dump inside this loop was tried and rejected: it appends ~30 lines per pass and
	 * the console scrolls the page off faster than it can be photographed, so the passes are one line. */
	for (unsigned pass = 1;; ++pass) {
		extern volatile int native_buttons_stats[10] __attribute__((weak_import));
		extern unsigned native_legacy_uptime_seconds(void) __attribute__((weak_import));
		extern void muyu_proc_census(char *, size_t) __attribute__((weak_import));
		unsigned now = 0, irq = 0, rep = 0, msk = 0, oth = 0, pdc = 0, sp = 0;
		int st = 0, pon = -1, gpio = -1, err = 0;

		IOSleep(5000);
		if (&native_legacy_uptime_seconds != NULL) {
			now = native_legacy_uptime_seconds();
		}
		if (&native_buttons_stats != NULL) {
			st = native_buttons_stats[0];
			irq = (unsigned)native_buttons_stats[1];
			rep = (unsigned)native_buttons_stats[2];
			msk = (unsigned)native_buttons_stats[3];
			pon = native_buttons_stats[4];
			gpio = native_buttons_stats[5];
			oth = (unsigned)native_buttons_stats[6];
			pdc = (unsigned)native_buttons_stats[7];
			err = native_buttons_stats[8];
			sp = (unsigned)native_buttons_stats[9];
		}
		printf("MUYU LIVE p=%u t=%u btn st=%d irq=%u rep=%u msk=0x%x pon=%d gpio=%d pdc=0x%x err=%d sp=%u oth=%u\n",
		    pass, now, st, irq, rep, msk, pon, gpio, pdc, err, sp, oth);
		if ((pass % 12) == 0 && &muyu_proc_census != NULL) {
			char census[160];
			muyu_proc_census(census, sizeof(census));
			printf("%s", census);
		}
	}
}

static void
muyu_dump_start(void)
{
	uint32_t at = 0;
	thread_t thread;
	/* First caller wins: the display probe calls this at ~t+10 s (early enough to take the panel before the
	 * graphics client rotates and shrinks the console) and load_init_program calls it again later. Without
	 * the guard the second call would start a second ticker on the same panel. */
	static int started;

	if (started) {
		return;
	}
	if (!PE_parse_boot_argn("muyu-dump-at", &at, (int)sizeof(at)) || at < 5) {
		return;
	}
	started = 1;
	if (kernel_thread_start_priority(muyu_dump_later, (void *)(uintptr_t)at, BASEPRI_KERNEL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}

void
muyu_dump_start_late(void)
{
	muyu_dump_start();
}
