/* Weak stand-ins for the sheng-only helpers BootLog.c still names outside compose().
 *
 * `__attribute__((weak))` on a mere declaration does not make a weak reference in a static Mach-O link, so
 * the kernel link failed with "symbol(s) not found" for these. A weak *definition* is the portable answer:
 * it links, and if a board ever provides the real helper the strong definition wins.
 *
 * Returning failure/null is what BootLog.c already treats as "this board does not have it": every caller
 * guards with `if (&helper)` and then checks the return value.
 */
#include <stdint.h>

__attribute__((weak)) int boot_reset_probe(char *text, unsigned cap) { (void)text; (void)cap; return -1; }
__attribute__((weak)) int boot_reset_mark_bootloader(char *text, unsigned cap) { (void)text; (void)cap; return -1; }
__attribute__((weak)) int boot_reset_watchdog(uint32_t seconds, char *text, unsigned cap)
{
    (void)seconds; (void)text; (void)cap; return -1;
}
__attribute__((weak)) int boot_reset_bootloader(char *text, unsigned cap) { (void)text; (void)cap; return -1; }
__attribute__((weak)) uint64_t boot_diag_scan(uint64_t offset) { (void)offset; return 0; }
__attribute__((weak)) int boot_diag_stream(int (*emit)(const void *, uint32_t, void *), void *context)
{
    (void)emit; (void)context; return -1;
}
__attribute__((weak)) const char *boot_progress_tag(void) { return "muyu"; }
