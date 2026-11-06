/* Draw a kernel panic message, with its arguments, on the frame buffer, then stop.
 *
 * Taken from UniXNU-uke/scripts/xnu-panic-screen.c (2026-11-06). uke boots the
 * same ipados-27.0.1-24A446 arm64e profile on a 3200x2136 pad panel, so its
 * renderer is the proven one for this target; the muyu copy had drifted into a
 * speculative 90-degree rotation, a 16-line band and a pointer-only %s, which is
 * why a panic photographed as overlapping text cut off after a few lines.
 *
 * Kept from uke, because each one answers a failure seen on hardware:
 *  - two passes: format the whole message into a stack buffer with drawing off,
 *    then clear the panel and draw the message alone from row 0 (a message drawn
 *    while the console is still painting collides with it);
 *  - quiet_console() sets disableConsoleOutput, so the other CPUs stop drawing
 *    the old log over the panic;
 *  - PANIC_TRACE_FRAMES walks >=1 bounded frame links and appends "BT <lr>" values,
 *    which is what attributes a fault to a function when the kernel is stripped;
 *  - PANIC_TEXT/PANIC_SHADOW/PANIC_FLUSH copies for a board that has them.
 *
 * Changed for muyu: %s prints <str 0x...> instead of the bytes when the string is
 * not provably readable. On 12377 a panic message carried a %s whose page passed
 * the AT S1E1R probe and the byte load then took a data abort inside this
 * function. A lost expansion costs one argument; a nested panic costs the screen.
 *
 * Built freestanding by patch-xnu-entry-probe.py and placed in unused padding
 * of a COPY of the kernel; panic() and its common panic funnel are redirected
 * here. It runs on the kernel's own mapping of the frame buffer.
 *
 * panic(fmt, ...) is variadic and the Darwin arm64 ABI passes every variadic
 * argument in an 8-byte stack slot, starting at the stack pointer on entry.
 * A small printf walks the format string over those slots (%s %x %p, with
 * # flag, 0-flag, width, precision and l/ll/z).
 */
typedef unsigned long u64;
typedef unsigned int u32;
typedef unsigned char u8;

struct vc_info {
  u32 v_height, v_width, v_depth, v_rowbytes;
  u64 v_baseaddr;
};

/* 3x cells: at 1x the panic text is unreadable on this panel. The full text does
 * not have to fit on screen -- it is staged to the pram and lands in userdata. */
#define SCALE   3

extern const u64 gFont[];

/* All state lives on the stack: this code sits in the kernel's read-only text,
 * so a writable global would fault on its first store. */
struct ctx {
  u8  *base;
  u32 rb, w, h, x, y;
  u8  *log;   /* buffer that also receives the formatted message, 0 = none */
  u32 logn, logcap;
};

#ifndef PANIC_TEXT
#define PANIC_TEXT 0UL
#endif
#ifndef UKE_PANIC_FLUSH
#define UKE_PANIC_FLUSH 0UL
#endif
/* disableConsoleOutput (kern/printf.c through the platform expert): the console keeps drawing from the other CPUs onto the same frame
 * buffer this screen draws on, so the panic text has to own the panel from here on. */
#ifndef PANIC_QUIET
#define PANIC_QUIET 0UL
#endif
/* The board's panic shadow (kits/uke/LegacyDisplayProbe.cpp): the message is copied there and the probe's heartbeat thread, which
 * keeps running on another CPU, prints it into the log. A plain copy, no variadic call: the Darwin ABI passes variadic arguments on
 * the stack and a call from here reached the kernel's printf with an empty string. */
#ifndef PANIC_SHADOW
#define PANIC_SHADOW 0UL
#endif
#ifndef PANIC_SHADOW_LEN
#define PANIC_SHADOW_LEN 0UL
#endif
#define PANIC_TEXT_BYTES 256
#ifndef PANIC_TRACE_FRAMES
#define PANIC_TRACE_FRAMES 0
#endif

__attribute__((noreturn)) static void park (void)
{
  for (;;) {
    __asm__ volatile ("wfe");
  }
}

static void put (struct ctx *g, u8 c)
{
  const u32 cell = 6 * SCALE, line = 14 * SCALE;
  u32 row, col, dy, dx;

  /* The banner/shadow copy exists for boards that publish such a buffer (uke's
   * BootProgress). muyu publishes neither symbol, so the whole path is compiled
   * out: on 2026-11-06 the gate was entered with a stale pointer (0x...b50, a
   * kernel stack address) and the byte store faulted inside this function, which
   * repainted the screen with the renderer's own data abort and hid the real
   * panic. A board without the symbols must not carry the code. */
#if PANIC_TEXT || PANIC_SHADOW
  if (g->log && g->logcap && g->logn < g->logcap - 1 && c >= 9 && c <= 126) {
    g->log[g->logn++] = c;
    g->log[g->logn] = 0;
  }
#endif

  if (c == '\n') {
    g->x = 0;
    g->y += line;
    return;
  }
  if (c == '\t') {
    g->x += 4 * cell;
    return;
  }
  if (c < 32 || c > 126) {
    return;
  }
  if (g->x + cell > g->w) {
    g->x = 0;
    g->y += line;
  }
  if (g->y + line > g->h) {
    return;
  }
  u64 glyph = gFont[c - 32];
  u32 top = (u32)(glyph >> 32), bottom = (u32)glyph;
  for (row = 0; row < 12; row++) {
    u32 data = (row < 6) ? (top >> (row * 5)) : (bottom >> ((row - 6) * 5));
    for (col = 0; col < 5; col++) {
      if ((data >> col) & 1) {
        for (dy = 0; dy < SCALE; dy++) {
          for (dx = 0; dx < SCALE; dx++) {
            *(volatile u32 *)(g->base + (u64)(g->y + row * SCALE + dy) * g->rb + (u64)(g->x + col * SCALE + dx) * 4) = 0xFFFFFFFFu;
          }
        }
      }
    }
  }
  g->x += cell;
}

static void number (struct ctx *g, u64 value, u32 base, u32 width, int zero)
{
  u8  digits[24];
  u32 n = 0, i;

  do {
    u32 d = (u32)(value % base);
    digits[n++] = (u8)(d < 10 ? '0' + d : 'a' + d - 10);
    value /= base;
  } while (value && n < 24);
  for (i = n; i < width; i++) {
    put (g, zero ? '0' : ' ');
  }
  while (n) {
    put (g, digits[--n]);
  }
}

/* Probe each string byte before dereferencing it. Heap strings (process name
 * and exit reason) live outside the linked image. AT performs a hardware
 * translation query only: no table edits, mappings, or exception bypass.
 * Refuse user pointers, translation faults and device-memory attributes. */
static int readable (const char *s, const u64 *args)
{
  u64 a = (u64)s, par;
  (void)args;
  if (a < 0xffff000000000000UL) return 0;
  __asm__ volatile ("at s1e1r, %1\n\tisb\n\tmrs %0, par_el1" : "=r" (par) : "r" (a) : "memory");
  return !(par & 1) && ((par >> 60) != 0);
}

/* Probe a row before writing it. muyu's vinfo can describe more rows than the frame
 * buffer the kernel actually mapped: clearing to v_height took a data abort inside
 * this renderer (2026-11-06, pc 0xfffffff00aeef944, esr 0x96000045, an EL1 write
 * translation fault). AT S1E1W asks the MMU the same question the store would, so the
 * clear stops at the first row that is not provably writable and the message still
 * gets the rows that are. */
static int writable (const u8 *p)
{
  u64 a = (u64)p, par;
  __asm__ volatile ("at s1e1w, %1\n\tisb\n\tmrs %0, par_el1" : "=r" (par) : "r" (a) : "memory");
  return !(par & 1);
}

/* Rows this renderer may write, and the width the row stride really holds. */
static u32 usable_height (struct ctx *g)
{
  u32 r;
  if (g->rb >= 4 && g->w * 4 > g->rb) {
    g->w = g->rb / 4;
  }
  for (r = 0; r < g->h; r++) {
    if (!writable (g->base + (u64)r * g->rb)) {
      return r;
    }
  }
  return g->h;
}

static void quiet_console (void)
{
#if PANIC_QUIET
  *(volatile int *)(PANIC_QUIET) = 1;
  __asm__ volatile("dsb sy" ::: "memory");
#endif
}

static void format (struct ctx *g, const char *fmt, const u64 *args)
{
  u32 i, used = 0;

  for (i = 0; i < 2000 && fmt[i]; i++) {
    u8 c = (u8)fmt[i];
    if (c != '%') {
      put (g, c);
      continue;
    }
    u32 width = 0, precision = 160;
    int zero = 0, longs = 0;
    c = (u8)fmt[++i];
    if (c == '0') {
      zero = 1;
      c = (u8)fmt[++i];
    }
    while (c >= '0' && c <= '9') {
      width = width * 10 + (c - '0');
      c = (u8)fmt[++i];
    }
    if (c == '.') {
      /* Only '%.Ns' is used in practice; anything else keeps the 160-byte default. */
      precision = 0;
      c = (u8)fmt[++i];
      while (c >= '0' && c <= '9') {
        if (precision < 200) precision = precision * 10 + (c - '0');
        c = (u8)fmt[++i];
      }
      if (precision == 0 || precision > 200) precision = 200;
    }
    if (width > 256) width = 256;
    while (c == 'l' || c == 'z' || c == 'h') {
      longs += (c != 'h');
      c = (u8)fmt[++i];
    }
    if (c == '%') {
      put (g, '%');
      continue;
    }
    if (c == 0) {
      break;
    }
    u64 v = args[used++];
    if (c == 's') {
      /* Expand the string only while every byte in it is proven readable, and
       * never past 160 characters. The check runs before each load, in this
       * order, because a %s that passed the AT S1E1R probe and then faulted on
       * the load is what took a nested data abort here on 12377. */
      const char *s = (const char *)v;
      u32 k = 0, ok = (s != 0);
      if (ok && precision < 160) {
        precision = 160;
      }
      while (ok && k < precision) {
        if (!readable (s + k, args)) {
          ok = 0;
          break;
        }
        if (s[k] == 0) {
          break;
        }
        k++;
      }
      if (ok) {
        for (u32 j = 0; j < k; j++) {
          put (g, (u8)s[j]);
        }
      } else if (s != 0) {
        put (g, '<');
        put (g, 's');
        put (g, 't');
        put (g, 'r');
        put (g, ' ');
        number (g, v, 16, 0, 0);
        put (g, '>');
      }
    } else if (c == 'x' || c == 'X') {
      number (g, longs ? v : (v & 0xFFFFFFFFu), 16, width, zero);
    } else if (c == 'u' || c == 'd') {
      number (g, longs ? v : (v & 0xFFFFFFFFu), 10, width, zero);
    } else if (c == 'p') {
      put (g, '0');
      put (g, 'x');
      number (g, v, 16, width, zero);
    }
  }
}

static void text (struct ctx *g, const char *s)
{
  while (*s) {
    put (g, (u8)*s++);
  }
}

static void hex (struct ctx *g, const char *label, u64 v)
{
  text (g, label);
  put (g, '0');
  put (g, 'x');
  number (g, v, 16, 16, 1);
}

/* x0 = fmt as for panic(); x1 is set by the stub to the entry stack pointer. */
__attribute__((noreturn, used, section(".text.b_main"))) void panic_screen_main (const char *fmt, const u64 *args)
{
  volatile struct vc_info *v = (volatile struct vc_info *)VINFO;
  struct ctx g;
  u32 row, col;

  __asm__ volatile ("msr daifset, #15");
  g.base = (u8 *)v->v_baseaddr;
  g.rb   = v->v_rowbytes;
  g.w    = v->v_width;
  g.h    = v->v_height;
  if (g.base == 0 || v->v_depth != 32 || fmt == 0) {
    park ();
  }

  /* Claim before clearing or formatting. This is normal kernel BSS, not a
   * device framebuffer: exclusive operations on device mappings may fault. */
#if PANIC_SHADOW_LEN
  u32 unclaimed = 0;
  if (!__atomic_compare_exchange_n((u32 *)PANIC_SHADOW_LEN, &unclaimed, 1, 0,
                                  __ATOMIC_RELAXED, __ATOMIC_RELAXED)) park ();
#endif
#if !PANIC_SHADOW_LEN
  volatile u32 *mark = (volatile u32 *)(g.base + (u64)(g.h - 1) * g.rb);
  if (mark[0] == 0xC0FFEE11u && mark[1] == 0x5CEEDA7Au) park ();
#endif
  g.h = usable_height (&g);
  if (g.h == 0) {
    park ();
  }
  g.x = 0;
  g.y = 0;
  /* Stream the message straight onto the panel. This renderer used to format the
   * whole message into a 1 KiB stack buffer first (as uke does), but this board's
   * kernel stack is small: a panic taken deep in the boot then put SP into the guard
   * page and the exception entry itself faulted, so the *first* panic's text was
   * lost and only "Invalid kernel stack pointer (probable overflow)" was drawn
   * (2026-11-06). Drawing needs no buffer, so none is taken. */
#if PANIC_TEXT || PANIC_SHADOW
  g.log = 0;
  g.logn = 0;
  g.logcap = 0;
#if PANIC_TEXT
  if (*(volatile u8 *)PANIC_TEXT == 0) {
    g.log = (u8 *)PANIC_TEXT;
    g.logcap = PANIC_TEXT_BYTES;
  }
#else
  g.log = (u8 *)PANIC_SHADOW;
  g.logcap = 1024;
#endif
#endif
  /* Quiesce the ordinary console, then take the whole writable panel: the message
   * starts at row zero, over the old top-of-screen log. */
  quiet_console ();
  for (row = 0; row < g.h; row++) {
    for (col = 0; col < g.w; col++) {
      *(volatile u32 *)(g.base + (u64)row * g.rb + (u64)col * 4) = 0;
    }
  }
  __asm__ volatile ("dsb sy");
#if !PANIC_SHADOW_LEN
  /* The clear has already erased the old marker, so claim again before drawing:
   * a renderer that starts on another CPU now parks instead of painting its glyphs
   * through this message (2026-11-06: the frame-trace line came out with two
   * renders' digits over each other). */
  mark[0] = 0xC0FFEE11u;
  mark[1] = 0x5CEEDA7Au;
  __asm__ volatile ("dsb sy");
#endif
  /* Geometry first, before the message: if anything below faults, the first line
   * still says which frame buffer, stride and panel this renderer was given. */
  put (&g, 'G');
  hex (&g, "", g.rb);
  hex (&g, " ", g.w);
  hex (&g, " ", g.h);
  put (&g, '\n');
  format (&g, fmt, args);
#if PANIC_TRACE_FRAMES
  /* Bounded frame links from this CPU: raw authenticated LR values, kept for
   * offline symbolization. Never follow an unchecked stack pointer. */
  {
    const u64 *fp = (const u64 *)__builtin_frame_address(0);
    u32 i;
    for (i = 0; i < PANIC_TRACE_FRAMES; ++i) {
      if (!readable ((const char *)fp, 0) || !readable ((const char *)(fp + 1), 0)) break;
      put (&g, '\n');
      put (&g, 'F');
      put (&g, (u8)('0' + (i % 10)));
      format (&g, " %p", fp + 1);
      const u64 next = fp[0], at = (u64)fp;
      if ((next & 15) || next <= at || next - at > 0x4000) break;
      fp = (const u64 *)next;
    }
  }
#endif
  __asm__ volatile ("dsb sy");
  /* First panic wins: a later one (another thread, or a fault inside this function)
   * finds these two pixels and parks instead of repainting over this message. */
#if !PANIC_SHADOW_LEN
  mark[0] = 0xC0FFEE11u;
  mark[1] = 0x5CEEDA7Au;
  __asm__ volatile ("dsb sy");
#endif
#if PANIC_SHADOW_LEN
  if (g.logn) {
    __atomic_store_n((u32 *)PANIC_SHADOW_LEN, g.logn, __ATOMIC_RELEASE);
  }
#endif
  park ();
}

/* Entry from panic(): keep the stack pointer, which addresses the variadic arguments. */
__attribute__((naked, noreturn, section(".text.a_entry"))) void panic_screen (const char *fmt)
{
  __asm__ volatile ("mov x1, sp\n\tb panic_screen_main");
}

/* The real panic_trap_to_debugger signature takes (fmt, va_list *, ...).
 * Darwin ARM64 va_list is a stack pointer; its address is passed in x1.
 * The wrapper has already established its frame, so SP is NOT the argument
 * list here. Follow exactly the one pointer supplied by the real wrapper.
 */
__attribute__((naked, noreturn, section(".text.a_common"))) void panic_screen_common (void)
{
  __asm__ volatile ("ldr x1, [x1]\n\tb panic_screen_main");
}

#include "Font.h"
__attribute__((section(".text.z_font"))) const u64 gFont[] = GLYPH_FONT;
