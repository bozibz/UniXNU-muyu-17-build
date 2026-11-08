/* rb: reboot(2) with a reason string, freestanding aarch64 Linux (no libc).
 *
 * Why not busybox reboot: busybox cannot pass a reason, and the reason is the whole point here.
 * Qualcomm's ABL maps the restart reason to a boot target ("bootloader" -> fastboot, "recovery" ->
 * recovery), and that is the only way to leave a stuck Linux boot without touching the power key -
 * which matters because a power cycle wipes the ramoops region that holds its last kernel log.
 *
 * usage: rb [reason]        default reason: bootloader
 */
static long
sc4(long n, long a, long b, long c, long d)
{
	register long x8 __asm__("x8") = n;
	register long x0 __asm__("x0") = a;
	register long x1 __asm__("x1") = b;
	register long x2 __asm__("x2") = c;
	register long x3 __asm__("x3") = d;

	__asm__ volatile("svc #0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2), "r"(x3) : "memory");
	return x0;
}

static void
exit0(void)
{
	sc4(94, 0, 0, 0, 0);            /* exit_group */
	for (;;) {
	}
}

void
start_c(unsigned long *sp)
{
	long argc = (long)sp[0];
	char **argv = (char **)&sp[1];
	const char *why = "bootloader";

	if (argc > 1 && argv[1] != 0 && argv[1][0] != 0) {
		why = argv[1];
	}
	/* reboot(LINUX_REBOOT_MAGIC1, LINUX_REBOOT_MAGIC2, LINUX_REBOOT_CMD_RESTART2, why) */
	sc4(142, 0xfee1dead, 672274793, 0xa1b2c3d4, (long)why);
	exit0();
}

__asm__(".globl _start\n"
    "_start:\n"
    "  mov x0, sp\n"
    "  bl start_c\n"
    "  b .\n");
