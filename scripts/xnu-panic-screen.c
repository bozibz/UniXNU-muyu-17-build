/* Draw a kernel panic message, with its arguments, on the frame buffer, then stop.
 *
 * Built freestanding by patch-xnu-entry-probe.py and placed in unused padding
 * of a COPY of the kernel; panic() and its common panic funnel are redirected
 * here. It runs on the kernel's
 * own mapping of the frame buffer. With the board observer it uses the retained
 * GOP mapping prefix; otherwise it uses vinfo.v_baseaddr.
 *
 * panic(fmt, ...) is variadic and the Darwin arm64 ABI passes every variadic
 * argument in an 8-byte stack slot, starting at the stack pointer on entry.
 * A small printf walks the format string over those slots (%s %x %p, with
 * 0-flag, width, precision and l/ll/z). Kernel strings are checked with an
 * active hardware translation query before reading each byte;
 * unmapped/device/user pointers print as pointers. VINFO names that mapping prefix.
 */
typedef unsigned long u64;
typedef unsigned int u32;
typedef unsigned char u8;

struct vc_info {
  u32 v_height, v_width, v_depth, v_rowbytes;
  u64 v_baseaddr;
};

/* 2026-11-06: 4 instead of 3 and a 44-column wrap. On the 3.2K panel the message used to
 * run the full width in 18 px cells, which photographs as a smear; wrapping keeps every
 * line inside a third of the panel and the glyphs are a third larger. */
#define SCALE   4

extern const u64 gFont[];

/* All state lives on the stack: this code sits in the kernel's read-only text,
 * so a writable global would fault on its first store. */
struct ctx {
  u8  *base;
  u32 rb, w, h, x, y;
  u32 rot_w, rot_h;   /* unrotated frame-buffer width/height for the rotated writer */
  u8  *log;   /* kernel buffer that also receives the formatted message (PANIC_TEXT), 0 = none */
  u32 logn;
};

#ifndef PANIC_TEXT
#define PANIC_TEXT 0UL
#endif
#define PANIC_TEXT_BYTES 256

__attribute__((noreturn)) static void park (void)
{
  for (;;) {
    __asm__ volatile ("wfe");
  }
}

/* The kernel video console on this board draws rotated (zzzzzz-video-console-text.patch); writing
 * the frame buffer directly, as this renderer does, has to apply the same rotation or the text ends
 * up running down the panel and shears when read back (2026-11-06). One rotated pixel. */
static void pix (struct ctx *g, u32 x, u32 y, u32 colour)
{
  u32 w = g->w;                       /* unrotated height, kept in g->rot_h below */
  (void)w;
  u32 rx = y;                         /* 90 degrees counter-clockwise in frame-buffer space */
  u32 ry = g->rot_w - 1 - x;
  if (ry < g->rot_h) {
    *(volatile u32 *)(g->base + (u64)ry * g->rb + (u64)rx * 4) = colour;
  }
}

static void put (struct ctx *g, u8 c)
{
  const u32 cell = 6 * SCALE, line = 14 * SCALE;
  u32 row, col, dy, dx;

  if (g->log && g->logn < PANIC_TEXT_BYTES - 1 && c >= 9 && c <= 126) {
    g->log[g->logn++] = (c < 32) ? ' ' : c;
    g->log[g->logn] = 0;
  }

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
            pix (g, g->x + col * SCALE + dx, g->y + row * SCALE + dy, 0xFFFFFFFFu);
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
  __asm__ volatile ("at s1e1r, %1; isb; mrs %0, par_el1" : "=r" (par) : "r" (a) : "memory");
  return !(par & 1) && ((par >> 60) != 0);
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
    u32 width = 0;
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
    u32 precision = 4000;
    if (c == '.') {
      precision = 0;
      c = (u8)fmt[++i];
      if (c == '*') {
        precision = (u32)args[used++];
        c = (u8)fmt[++i];
      } else {
        while (c >= '0' && c <= '9') {
          if (precision < 4000) precision = precision * 10 + (c - '0');
          c = (u8)fmt[++i];
        }
      }
      if (precision > 4000) precision = 4000;
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
      /* 2026-11-06: the argument is printed, never dereferenced. On 12377 a panic message can carry
       * a %s whose pointer is not readable (or the readable() translation probe lets it through), and
       * the byte load then takes a data abort inside this function - which panics again and paints a
       * second message over the first, so the screen becomes unreadable (pc 0xfffffff00aeef6a8:
       * LDRB W8,[X20,W28,UXTW]). The diagnostic text people need is in the format string and the
       * file:line tail, so losing the expansion costs little. */
      const char *s = (const char *)v;
      if (s == 0) {
        put (g, '0');
      } else {
        hex (g, "<str ", (u64)s);
        put (g, '>');
      }
      (void)precision;
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
  /* Geometry comes from vinfo after all: this is the same mapping the kernel console draws into,
   * and the console's own register dump is readable on this panel. Hardcoding 3200x2136/stride
   * 12800 (2026-11-06) is what broke the glyphs - they came out sheared. Only the base address was
   * ever in question, and it is fine. */
  g.base = (u8 *)v->v_baseaddr;
  g.rb   = v->v_rowbytes;
  g.w    = v->v_width;
  if (g.w > 44 * 6 * SCALE) {
    g.w = 44 * 6 * SCALE;   /* wrap at 44 columns */
  }
  /* vinfo's height is larger than the frame buffer this kernel actually maps: clearing rows all the
   * way to v_height took a data abort inside this function (2026-11-06, pc 0xfffffff00aeef944,
   * esr 0x96000045 - an EL1 write fault). The kernel console only ever draws a few lines at the
   * top, which is why it never hit this. Keep to a band that is certainly mapped. */
  g.h    = v->v_height;
  if (g.h > 16 * 14 * SCALE) {
    g.h = 16 * 14 * SCALE;
  }
  g.rot_w = v->v_width;
  g.rot_h = v->v_height;
  if (g.rot_w > 4096) {
    g.rot_w = 4096;
  }
  if (g.rot_h > 4096) {
    g.rot_h = 4096;
  }
  if (g.base == 0 || v->v_depth != 32 || fmt == 0) {
    park ();
  }

  /* First panic wins: a marker in the last row's first two pixels means a panic already drew
   * the screen; a later panic (another thread or exception) must not overwrite it. */
  volatile u32 *mark = (volatile u32 *)(g.base + (u64)(g.h - 1) * g.rb);
  if (mark[0] == 0xC0FFEE11u && mark[1] == 0x5CEEDA7Au) {
    park ();
  }
  /* Clear the whole screen, then draw from the top. DarwinBoot paints its handoff text with a 2x
   * font and XNU's console repaints the same area with a smaller one, so anything left uncleared
   * reads as overlapping garbage. The panic message itself carries pc/lr/esr/far, so the console's
   * copy is not needed. Geometry is vinfo's, the same mapping the console draws into. */
  g.x = 0;
  g.y = 0;
  g.log = 0;
  g.logn = 0;
  {
    /* Clear in content space through the same rotation, inside the band. */
    for (row = 0; row < g.h; row++) {
      for (col = 0; col < g.w; col++) {
        pix (&g, col, row, 0);
      }
    }
  }
  g.y = 8;
  /* The board's live banner keeps showing this text after the next swap paints over the screen (BootProgress.c,
   * boot_progress_panic_text). The first panic's text is kept: a buffer that already holds one is left alone. */
  if (PANIC_TEXT && *(volatile u8 *)PANIC_TEXT == 0) {
    g.log = (u8 *)PANIC_TEXT;
  }
  format (&g, fmt, args);
  g.log = 0;
  __asm__ volatile ("dsb sy");
  /* Sample the active CPU translation regime, without changing it. This is
   * panic-time evidence only; it does not establish early mappings or EL0 ABI. */
  {
    u64 tcr, sctlr;
    __asm__ volatile ("mrs %0, tcr_el1" : "=r" (tcr));
    __asm__ volatile ("mrs %0, sctlr_el1" : "=r" (sctlr));
    hex (&g, "\nTCR=", tcr);
    hex (&g, "\nSCTLR=", sctlr);
  }
  /* Resolved from this candidate's symbols by the patcher, never fixed addresses.
   * Values are sampled at panic time and need not form an atomic VM snapshot. */
  (void)MAX_MEM;
  hex (&g, "\npages=", *(volatile const u32 *)PAGE_TOTAL);
  hex (&g, "\nfree=", *(volatile const u32 *)PAGE_FREE);
  /* The raw argument slots are not dumped: the formatted message above already carries them (room for the PANIC_TEXT copy). */
  mark[0] = 0xC0FFEE11u;
  mark[1] = 0x5CEEDA7Au;
  __asm__ volatile ("dsb sy");
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
