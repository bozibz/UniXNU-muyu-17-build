/*
 * muyu: experimental CDC ACM debug console (u128).
 * Ported from ios-vermeer-lab/xnu/vermeer/vermeer_usb_debug.c, where the same driver was proven
 * on SM8550 and SM8650.  Opt-in: muyu_usb=1.
 *
 * Why this exists: the panel can only be photographed, and the console dies exactly when it is
 * most interesting (HANDOFF-2026-11-07.md §47).  This port reuses ABL's PHY/clocks/SMMU and
 * streams the console mirror over USB, so the host reads the log instead of the camera.
 *
 * It never changes GCC, SMMU, PMIC or storage.  For this first experiment the two SoC guards of
 * the original (GCC clock state, SMMU identity route) are *printed* but not enforced, because the
 * expected cliffs values are not known yet; the DWC3 and DMA guards are enforced.
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <kern/clock.h>
#include <kern/kalloc.h>
#include <kern/debug.h>
#include <kern/thread.h>
#include <mach/thread_act.h>
#include "build_tag.h"
#include <kern/sched_prim.h>
#include <machine/machine_routines.h>
#include <arm/caches_internal.h>
#include <vm/pmap.h>
#include <vm/vm_kern.h>
#include <vm/vm_kern_xnu.h>
#include <pexpert/pexpert.h>

#include "muyu_usb_protocol.h"

extern vm_offset_t ml_io_map(vm_offset_t, vm_size_t);
extern ppnum_t pmap_find_phys(pmap_t, addr64_t);
extern unsigned muyu_debug_copy(uint64_t *, char *, unsigned);
extern uint64_t muyu_debug_total(void);
extern uint64_t muyu_debug_dropped(void);
void muyu_usb_debug_start(void);
void muyu_usb_report(void);
void muyu_vc_status_repaint(void);
int muyu_vc_logdump(void);

#define GCTL 0xc110
#define GEVNTADRLO 0xc400
#define GEVNTADRHI 0xc404
#define GEVNTSIZ 0xc408
#define GEVNTCOUNT 0xc40c
#define DCFG 0xc700
#define DCTL 0xc704
#define DEVTEN 0xc708
#define DSTS 0xc70c
#define DALEPENA 0xc720
#define EPBASE(n) (0xc800 + 16 * (n))
#define GUSB2PHYCFG(n) (0xc200 + 4 * (n))
#define GUSB3PIPECTL(n) (0xc2c0 + 4 * (n))
#define RUN (1u << 31)
#define RESET (1u << 30)
#define HALTED (1u << 22)
#define ACTIVE (1u << 10)
#define FORCE_REMOVE (1u << 11)
#define SET_CONFIG 1
#define SET_RESOURCE 2
#define STALL 4
#define CLEAR_STALL 5
#define START 6
#define END 8
#define START_CONFIG 9
#define T_NORMAL (1u << 4)
#define T_SETUP (2u << 4)
#define T_STATUS2 (3u << 4)
#define T_STATUS3 (4u << 4)
#define T_DATA (5u << 4)
#define HWO 1u
#define TRB_FLAGS (HWO | 2u | (1u << 10) | (1u << 11))
#define EVENTS 0
#define EVENT_SIZE 4096
/* Separate cache lines: hardware never shares a dirty CPU-owned TRB line. */
#define SETUP 0x1200
#define DATA 0x1300
#define NOTIFY 0x1500
#define RX 0x1800
#define TX 0x2000
#define RX_SIZE 512
#define TX_SIZE 2048
#define DMA_SIZE 0x4000

/* cliffs (SM8635) addresses, from the port's own device tree and the qcom,dwc-usb3-msm layout. */
#define MUYU_DWC3_BASE 0xa600000
#define MUYU_DWC3_SIZE 0xe000
#define MUYU_QSCRATCH_BASE 0xa6f8800
#define MUYU_QSCRATCH_SIZE 0x1000
#define MUYU_GCC_BASE 0x100000
#define MUYU_GCC_SIZE 0x1f4200   /* reg = <0x100000 0x1f4200> in this board's device tree */
#define MUYU_APPS_SMMU_BASE 0x15000000
#define MUYU_APPS_SMMU_SIZE 0xd0000

static const unsigned trb_off[6] = { 0x1000, 0x1040, 0x1080, 0x10c0, 0x1100, 0x1140 };
static const char *const usb_strings[] = { NULL, "Muyu Lab", "Muyu XNU Debug", "MUYUDBG001" };

static struct {
	vm_offset_t regs, dma, smmu;
	uint64_t pa, cursor, tx_bytes, rx_bytes;
	uint32_t pos, events, setups, mps;
	unsigned resource[6], requested[6];
	bool busy[6], failed, configured, dtr, notify;
	enum { SETUP_PHASE, DATA_PHASE, WAIT2, WAIT3, STATUS_PHASE } phase;
	bool data_in, set_line;
	uint8_t coding[7];
	char command[64];
	unsigned command_n;
	bool command_overflow;
	int sleep_channel;
} u;

static uint32_t
read_reg(vm_offset_t base, unsigned off)
{
	uint32_t v = *(volatile uint32_t *)(base + off);

	__asm__ volatile ("dmb oshld" ::: "memory");
	return v;
}

static uint32_t
rd(unsigned off)
{
	return read_reg(u.regs, off);
}

/* u136: only used for the USB power/clock block, which is not part of the controller. */
static void
write_reg(vm_offset_t base, unsigned off, uint32_t v)
{
	*(volatile uint32_t *)(base + off) = v;
	__asm__ volatile ("dsb sy" ::: "memory");
}

static void
wr(unsigned off, uint32_t v)
{
	*(volatile uint32_t *)(u.regs + off) = v;
	__asm__ volatile ("dsb sy" ::: "memory");
}

static void
sync_dma(unsigned off, unsigned size)
{
	flush_dcache64((addr64_t)(u.pa + off), size, TRUE);
	__asm__ volatile ("dsb sy" ::: "memory");
}

/*
 * u129: progress readout that survives a scrolling/blocked console.
 *
 * The console mirrors nothing we can read back on the host (that is the whole point of this
 * driver), and the panel only ever shows the last ~28 lines, so any diagnostic we print in
 * bsd_init() is scrolled away long before the interesting part of the boot.  So instead of
 * relying on a line, the driver records a single number plus the name of the last failed step,
 * and the caller can turn that into a one-shot panic (boot-arg muyu_usb_panic=1): a panic halts
 * the machine, which is the only screen content that is guaranteed not to move.
 */
static volatile int muyu_usb_stage;
static const char *muyu_usb_last = "none";
/* u145: 0 = no low DMA block at all, 1 = 16 KiB below 4 GiB, 2 = 8 KiB, 3 = 4 KiB. */
static int muyu_usb_dma_code;
/* u146: the two numbers that decide whether the ring address is even acceptable.  They are
 * painted as one two-digit value: hi_read * 2 + (reset completed ? 1 : 0). */
static volatile int muyu_usb_reset_ok;
static volatile int muyu_usb_ring_ok;
static volatile int muyu_usb_ring_hi_read;

/* u151: one photo has to answer "did the guard land, did the reset finish, which endpoint
 * parameter set the core accepts".  The three-digit panel cannot carry that, so the values are
 * kept here and painted as a table at the end of the attempt (see usb_table()). */
static uint32_t t_phy_in, t_phy_cmd, t_pipe_in, t_cmd_pre;
static uint32_t t_dalep_in;
static int t_probe[4] = { -100, -100, -100, -100 };

/*
 * u143: once the boot reaches launchd the panel belongs to userspace, and the driver's own
 * lines are pushed off in seconds - four photo rounds in a row read the *tail* of the boot and
 * never the one number we need.  So the stage stops being a log line and becomes a pair of
 * digits painted straight into the framebuffer by the heartbeat, where no amount of scrolling
 * can overwrite them.  Draw order is: stage, then a separator, then the failure id.
 */
int
muyu_usb_stage_value(void)
{
	return muyu_usb_stage;
}

/* 0 = none, otherwise the index of the last failed step, so the panel can name the failure
 * without spelling it out. */
int
muyu_usb_last_id(void)
{
	static const char *const names[] = {
		"none",
		"boot-arg not seen", "already attempted", "dwc3 map", "not dwc3",
		"dma alloc", "dma lookup", "dma align", "dma width", "dma not contiguous",
		"qscratch map", "soft reset stuck, continuing", "ring address rejected",
		"ep0 start config", "ep0 set config", "ep0 set resource",
		"ep1 set config", "ep1 set resource", "setup arm",
	};
	/* u145: when the block below 4 GiB does not fit the 16 KiB layout the boot stops at the
	 * allocation step, which would otherwise look exactly like "dma alloc failed".  Report the
	 * size that *did* fit instead: 20 = nothing at all, 22 = 8 KiB, 23 = 4 KiB. */
	if (muyu_usb_dma_code != 1) {
		return 20 + muyu_usb_dma_code;
	}
	for (unsigned int i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
		if (muyu_usb_last == names[i]) {
			return (int)i;
		}
	}
	return -1;
}

/* u146: third panel value.  low bit = "the soft reset finished", upper digits = the HI word the
 * core actually kept after GEVNTADRHI was written.  Reset ok + HI=8 -> 17; reset stuck + HI=0 -> 0;
 * reset ok + HI=0 -> 1, which is the "32-bit core" answer. */
int
muyu_usb_ring_code(void)
{
	return (muyu_usb_ring_hi_read & 0x1f) * 2 + (muyu_usb_reset_ok ? 1 : 0);
}

static void
fail(const char *why)
{
	if (!u.failed) {
		muyu_usb_last = why;
		/* u138: every build so far ended on the same sentence, so the panel could not tell two
		 * kernels apart, and the panel only ever shows the last line.  The stop reason now
		 * carries the build tag and the live controller state in that one line. */
		printf("MUYU USB" MUYU_TAG ": stopped: %s | dcfg=%08x dctl=%08x dsts=%08x gctl=%08x phy2=%08x fifo0=%08x\n",
		    why, u.regs ? rd(DCFG) : 0, u.regs ? rd(DCTL) : 0,
		    u.regs ? rd(DSTS) : 0, u.regs ? rd(GCTL) : 0,
		    u.regs ? rd(GUSB2PHYCFG(0)) : 0, u.regs ? rd(0xc100) : 0);
	}
	u.failed = true;
}

/* u151: one endpoint command with the PHY guard of programming-guide section 3.2.2 around it.
 * Returns 0 when the core reports status 0, 1..15 for a CMD_STATUS error, and 16 when CMDACT is
 * still set after the whole budget - the "accepted but never executed" reading of u138..u151.
 * Never calls fail(), so a caller can try several parameter sets in one boot. */
static int
depcmd_guarded(unsigned ep, unsigned cmd, uint32_t p0, uint32_t p1, uint32_t p2,
    unsigned poll_us, uint32_t *last)
{
	uint32_t phy = rd(GUSB2PHYCFG(0));
	uint32_t pipe = rd(GUSB3PIPECTL(0));
	int rc = 16;

	wr(GUSB2PHYCFG(0), phy & ~((1u << 6) | (1u << 8)));
	wr(GUSB3PIPECTL(0), pipe & ~((1u << 17) | (1u << 16)));
	__asm__ volatile ("dsb sy" ::: "memory");

	wr(EPBASE(ep) + 8, p0);
	wr(EPBASE(ep) + 4, p1);
	wr(EPBASE(ep), p2);
	wr(EPBASE(ep) + 12, cmd | ACTIVE);
	*last = 0;
	for (unsigned n = 0; n < poll_us / 10; n++) {
		uint32_t v = rd(EPBASE(ep) + 12);

		*last = v;
		if (!(v & ACTIVE)) {
			rc = (int)((v >> 12) & 15);
			break;
		}
		delay(10);
	}
	wr(GUSB2PHYCFG(0), rd(GUSB2PHYCFG(0)) | (phy & ((1u << 6) | (1u << 8))));
	wr(GUSB3PIPECTL(0), rd(GUSB3PIPECTL(0)) | (pipe & ((1u << 17) | (1u << 16))));
	return rc;
}

static int
epcmd(unsigned ep, unsigned cmd, uint32_t p0, uint32_t p1, uint32_t p2)
{
	if (u.failed || ep >= 6) {
		return -1;
	}
	unsigned op = cmd & 15;
	uint32_t v = 0;

	/* u151: the guard is applied per command by depcmd_guarded().  Two attempts are kept for the
	 * command this port has never seen complete. */
	if (ep == 0) {
		t_cmd_pre = rd(EPBASE(0) + 12);
	}
	for (unsigned attempt = 0; attempt < 2; attempt++) {
		int rc = depcmd_guarded(ep, cmd, p0, p1, p2, 50000, &v);

		if (rc == 0) {
			if (op == START_CONFIG) {
				u.resource[ep] = (v >> 17) & 0x3f;
			}
			printf("MUYU USB" MUYU_TAG ": ep%u c%u ok reg=%08x p0=%08x p1=%08x pre=%08x\n",
			    ep, op, v, p0, p1, t_cmd_pre);
			return 0;
		}
		printf("MUYU USB" MUYU_TAG ": ep%u c%u attempt %u rc=%d reg=%08x p0=%08x p1=%08x pre=%08x\n",
		    ep, op, attempt, rc, v, p0, p1, t_cmd_pre);
		if (rc != 16 || op != SET_CONFIG) {
			break;
		}
		delay(100000);
	}
	{
		static char why[48];

		snprintf(why, sizeof(why), "endpoint cmd timeout ep%u c%u", ep, op);
		fail(why);
	}
	return -1;
}

/* u151: a page built for this one question, instead of a kernel log the console has already
 * scrolled past.  Ten labelled rows: what the PHY registers were, what they became, whether the
 * soft reset finished, what the endpoint command registers hold, and the result of each of the
 * four DEPCFG parameter sets tried this boot.  Painted through the same 8x8 dump that u148
 * introduced, then frozen, so nothing can scroll it away. */
static void
usb_table(void)
{
	printf("\n");
	printf("MUYU " MUYU_BUILD_TAG " TABLE =================\n");
	printf("GCTL %08x pc %d cg %d\n", rd(GCTL), (rd(GCTL) >> 12) & 3, rd(GCTL) & 1);
	printf("DCFG %08x DSTS %08x\n", rd(DCFG), rd(DSTS));
	printf("PHY2 in %08x cmd %08x now %08x\n",
	    t_phy_in, t_phy_cmd, rd(GUSB2PHYCFG(0)));
	printf("PIPE in %08x now %08x\n", t_pipe_in, rd(GUSB3PIPECTL(0)));
	printf("RST %d RING %d HI %d\n", muyu_usb_reset_ok, muyu_usb_ring_ok, muyu_usb_ring_hi_read);
	printf("DALEP in %08x now %08x\n", t_dalep_in, rd(DALEPENA));
	printf("DEPCMD0 pre %08x now %08x\n", t_cmd_pre, rd(EPBASE(0) + 12));
	printf("DEPCMD1 %08x DALEP %08x\n", rd(EPBASE(1) + 12), rd(DALEPENA));
	printf("P %d %d %d %d  (0 ok / 16 active)\n",
	    t_probe[0], t_probe[1], t_probe[2], t_probe[3]);
	printf("=================================\n");
	for (int k = 0; k < 10; k++) {
		if (muyu_vc_logdump()) {
			break;
		}
		delay(20000);
	}
	muyu_vc_status_repaint();
}

static void
transfer(unsigned ep, unsigned buf, unsigned length, unsigned kind)
{
	if (u.failed) {
		return;
	}
	if (ep >= 6 || u.busy[ep] || buf > DMA_SIZE || length > DMA_SIZE - buf) {
		fail("transfer ownership/bounds");
		return;
	}
	unsigned off = trb_off[ep];
	uint64_t p = u.pa + buf, t = u.pa + off;
	volatile uint32_t *trb = (volatile uint32_t *)(u.dma + off);

	trb[0] = (uint32_t)p;
	trb[1] = (uint32_t)(p >> 32);
	trb[2] = length;
	trb[3] = kind | TRB_FLAGS;
	u.requested[ep] = length;
	if (length) {
		sync_dma(buf, length);
	}
	sync_dma(off, 16);
	u.busy[ep] = true;
	(void)epcmd(ep, START, (uint32_t)(t >> 32), (uint32_t)t, 0);
}

static int
complete(unsigned ep)
{
	if (ep >= 6 || !u.busy[ep]) {
		fail("unexpected completion");
		return -1;
	}
	sync_dma(trb_off[ep], 16);
	volatile uint32_t *t = (volatile uint32_t *)(u.dma + trb_off[ep]);
	unsigned left = t[2] & 0xffffff;

	if ((t[3] & HWO) || (t[2] >> 28) || left > u.requested[ep]) {
		fail("invalid DMA completion");
		return -1;
	}
	u.busy[ep] = false;
	return (int)(u.requested[ep] - left);
}

static void
setup_arm(void)
{
	u.phase = SETUP_PHASE;
	transfer(0, SETUP, 8, T_SETUP);
}

static void
stall(void)
{
	(void)epcmd(0, STALL, 0, 0, 0);
	setup_arm();
}

static void
receive_arm(void)
{
	transfer(2, RX, RX_SIZE, T_NORMAL);
}

static void
stop_data(void)
{
	for (unsigned ep = 2; ep < 6; ep++) {
		if (u.busy[ep]) {
			if (!u.resource[ep] || epcmd(ep, END | FORCE_REMOVE | (u.resource[ep] << 16), 0, 0, 0)) {
				return;
			}
			u.busy[ep] = false;
		}
	}
	wr(DALEPENA, 3);
	u.configured = false;
	u.dtr = false;
	u.notify = false;
}

static void
configure(void)
{
	if (u.configured || u.failed) {
		return;
	}
	if (epcmd(0, START_CONFIG | (2u << 16), 0, 0, 0) ||
	    epcmd(2, SET_CONFIG, (2u << 1) | (u.mps << 3), (1u << 8) | (2u << 25), 0) ||
	    epcmd(2, SET_RESOURCE, 1, 0, 0) ||
	    epcmd(3, SET_CONFIG, (2u << 1) | (u.mps << 3) | (1u << 17), (1u << 8) | (3u << 25), 0) ||
	    epcmd(3, SET_RESOURCE, 1, 0, 0) ||
	    epcmd(5, SET_CONFIG, (3u << 1) | (16u << 3) | (2u << 17), (1u << 8) | (8u << 16) | (5u << 25), 0) ||
	    epcmd(5, SET_RESOURCE, 1, 0, 0)) {
		return;
	}
	wr(DALEPENA, 3 | (1u << 2) | (1u << 3) | (1u << 5));
	u.configured = true;
	receive_arm();
	printf("MUYU USB: CDC ACM configured, bulk MPS %u\n", u.mps);
}

static unsigned
string_desc(unsigned index, uint8_t *data)
{
	if (!index) {
		data[0] = 4; data[1] = 3; data[2] = 9; data[3] = 4;
		return 4;
	}
	if (index >= sizeof(usb_strings) / sizeof(usb_strings[0])) {
		return 0;
	}
	unsigned n = 2;

	for (const char *s = usb_strings[index]; *s && n < 126; s++) {
		data[n++] = (uint8_t)*s;
		data[n++] = 0;
	}
	data[0] = (uint8_t)n;
	data[1] = 3;
	return n;
}

static void
handle_setup(void)
{
	sync_dma(SETUP, 8);
	const uint8_t *s = (const uint8_t *)(u.dma + SETUP);
	uint8_t *data = (uint8_t *)(u.dma + DATA);
	unsigned value = s[2] | (s[3] << 8), index = s[4] | (s[5] << 8);
	unsigned length = s[6] | (s[7] << 8), n = 0;
	int kind = vusb_request(s[0], s[1], (uint16_t)value, (uint16_t)index, (uint16_t)length);

	u.data_in = (s[0] & 0x80) != 0;
	u.set_line = false;
	if (u.setups++ < 40) {
		printf("MUYU USB: setup %02x %02x %04x %04x len%u\n", s[0], s[1], value, index, length);
	}
	switch (kind) {
	case VUSB_DESC:
		switch (value >> 8) {
		case 1:
			if (value & 255) {
				break;
			}
			memcpy(data, vusb_device, sizeof(vusb_device));
			n = sizeof(vusb_device);
			break;
		case 2:
		case 7:
			if (value & 255) {
				break;
			}
			memcpy(data, vusb_config, sizeof(vusb_config));
			n = sizeof(vusb_config);
			data[1] = (uint8_t)(value >> 8);
			/* Configuration and other-speed configuration have opposite MPS. */
			for (unsigned i = 0; i < n; i += data[i]) {
				if (data[i + 1] == 5 && (data[i + 3] & 3) == 2) {
					unsigned m = (value >> 8) == 7 ? (u.mps == 512 ? 64 : 512) : u.mps;
					data[i + 4] = (uint8_t)m;
					data[i + 5] = (uint8_t)(m >> 8);
				}
			}
			break;
		case 3:
			n = string_desc(value & 255, data);
			break;
		case 6:
			if (!(value & 255)) {
				memcpy(data, vusb_qualifier, sizeof(vusb_qualifier));
				n = sizeof(vusb_qualifier);
			}
			break;
		default:
			break;
		}
		if (!n) {
			stall();
			return;
		}
		break;
	case VUSB_ADDRESS:
		wr(DCFG, (rd(DCFG) & ~(127u << 3)) | (value << 3));
		break;
	case VUSB_CONFIG:
		/* Reconfiguration while transfers are live is stopped, never recycled
		 * before a controller reset drains stale completion events. */
		if (!value && u.configured) {
			fail("host deconfigured; reboot to reconnect");
			return;
		}
		if (value) {
			configure();
		}
		break;
	case VUSB_GET_CONFIG:
		data[0] = u.configured ? 1 : 0;
		n = 1;
		break;
	case VUSB_STATUS:
		data[0] = data[1] = 0;
		n = 2;
		break;
	case VUSB_INTERFACE:
		break;
	case VUSB_GET_INTERFACE:
		data[0] = 0;
		n = 1;
		break;
	case VUSB_SET_LINE:
		u.set_line = true;
		break;
	case VUSB_GET_LINE:
		memcpy(data, u.coding, 7);
		n = 7;
		break;
	case VUSB_CONTROL:
		if (u.configured) {
			bool old = u.dtr;

			u.dtr = (value & 1) != 0;
			u.notify = true;
			if (u.dtr && !old) {
				printf("MUYU USB: host connected; the console mirror streams from here\n");
			}
		}
		break;
	case VUSB_BREAK:
		break;
	case VUSB_CLEAR_HALT:
		/* Active transfer replay cannot be made safe by CLEARSTALL alone. */
		if (u.busy[(index & 15) * 2 + ((index >> 7) & 1)]) {
			fail("clear-halt on active endpoint");
			return;
		}
		(void)epcmd((index & 15) * 2 + ((index >> 7) & 1), CLEAR_STALL, 0, 0, 0);
		break;
	default:
		stall();
		return;
	}
	if (u.failed) {
		return;
	}
	if (!length) {
		u.phase = WAIT2;
		return;
	}
	u.phase = DATA_PHASE;
	if (u.data_in) {
		transfer(1, DATA, n < length ? n : length, T_DATA);
	} else if (u.set_line) {
		transfer(0, DATA, 64, T_DATA);
	} else {
		stall();
	}
}

static void
command_line(void)
{
	u.command[u.command_n] = 0;
	if (u.command_overflow) {
		printf("MUYU DEBUG: command too long\n");
	} else if (!strcmp(u.command, "ping")) {
		printf("MUYU DEBUG: pong MUYUDBG001\n");
	} else if (!strcmp(u.command, "status")) {
		printf("MUYU DEBUG: MUYUDBG001 events=%u tx=%llu rx=%llu log=%llu dropped=%llu\n",
		    u.events, u.tx_bytes, u.rx_bytes, muyu_debug_total(), muyu_debug_dropped());
	} else if (!strcmp(u.command, "replay")) {
		u.cursor = 0;
		printf("MUYU DEBUG: replay retained RAM log\n");
	} else if (u.command_n) {
		printf("MUYU DEBUG: commands help status ping replay (queries only)\n");
	}
	u.command_n = 0;
	u.command_overflow = false;
}

static void
receive_done(unsigned length)
{
	sync_dma(RX, RX_SIZE);
	u.rx_bytes += length;
	const uint8_t *p = (const uint8_t *)(u.dma + RX);

	for (unsigned i = 0; i < length; i++) {
		if (p[i] == '\r' || p[i] == '\n') {
			command_line();
		} else if (p[i] >= 32 && p[i] <= 126) {
			if (u.command_n < sizeof(u.command) - 1) {
				u.command[u.command_n++] = (char)p[i];
			} else {
				u.command_overflow = true;
			}
		}
	}
	receive_arm();
}

static void
transmit(void)
{
	if (!u.configured || !u.dtr || u.failed) {
		return;
	}
	if (u.notify && !u.busy[5]) {
		const uint8_t msg[10] = { 0xa1, 0x20, 0, 0, 0, 0, 2, 0, 3, 0 };

		memcpy((void *)(u.dma + NOTIFY), msg, sizeof(msg));
		u.notify = false;
		transfer(5, NOTIFY, sizeof(msg), T_NORMAL);
	}
	if (u.busy[3]) {
		return;
	}
	unsigned n = muyu_debug_copy(&u.cursor, (char *)(u.dma + TX), TX_SIZE);

	/* Serial stream must have a short packet, with no padding/invented bytes. */
	if (n && !(n % u.mps)) {
		n--;
		u.cursor--;
	}
	if (n) {
		transfer(3, TX, n, T_NORMAL);
		u.tx_bytes += n;
	}
}

static void
event(uint32_t e)
{
	if (e & 1) {
		if ((e & 0xfe) != 0) {
			fail("unexpected device event format");
			return;
		}
		unsigned type = (e >> 8) & 15;

		if (type == 1) {
			if (u.configured) {
				stop_data();
				fail("bus reset after configuration; reboot to reconnect");
				return;
			}
			wr(DCFG, rd(DCFG) & ~(127u << 3));
			printf("MUYU USB: bus reset\n");
		} else if (type == 2) {
			unsigned speed = rd(DSTS) & 7;

			if (speed != 0 && speed != 1 && speed != 3) {
				fail("unexpected non-USB2 speed");
				return;
			}
			u.mps = speed == 0 ? 512 : 64;
			(void)epcmd(0, SET_CONFIG, (64u << 3) | (2u << 30), (1u << 8) | (1u << 10), 0);
			(void)epcmd(1, SET_CONFIG, (64u << 3) | (2u << 30), (1u << 8) | (1u << 10) | (1u << 25), 0);
			printf("MUYU USB: connected speed=%u\n", speed);
		} else if (!type && u.configured) {
			fail("cable disconnect; reboot to reconnect");
		} else if (type == 9 || type == 11) {
			fail("controller/event overflow");
		}
		return;
	}
	unsigned ep = (e >> 1) & 31, type = (e >> 6) & 15, status = (e >> 12) & 15;

	if (type == 1) {
		int n = complete(ep);

		if (n < 0) {
			return;
		}
		if (status & 1) {
			fail("USB transfer bus error");
			return;
		}
		if (ep == 2) {
			receive_done((unsigned)n);
			return;
		}
		if (ep > 1) {
			return;
		}
		switch (u.phase) {
		case SETUP_PHASE:
			if (ep != 0 || n != 8) {
				fail("setup length");
			} else {
				handle_setup();
			}
			break;
		case DATA_PHASE:
			if (u.set_line) {
				if (ep != 0 || n != 7) {
					fail("line coding length");
					break;
				}
				sync_dma(DATA, 64);
				memcpy(u.coding, (void *)(u.dma + DATA), 7);
			}
			u.phase = WAIT3;
			break;
		case STATUS_PHASE:
			setup_arm();
			break;
		default:
			fail("control completion order");
			break;
		}
	} else if (ep <= 1 && type == 3 && status == 2 && (u.phase == WAIT2 || u.phase == WAIT3)) {
		unsigned direction = u.phase == WAIT2 ? 1 : (u.data_in ? 0 : 1);

		if (ep != direction) {
			fail("control status direction");
			return;
		}
		unsigned kind = u.phase == WAIT3 ? T_STATUS3 : T_STATUS2;

		u.phase = STATUS_PHASE;
		transfer(ep, DATA, 0, kind);
	}
}

static void
usb_thread(void *arg __unused, wait_result_t result __unused)
{
	while (!u.failed) {
		unsigned count = rd(GEVNTCOUNT) & 0xffff;

		if (count > EVENT_SIZE || (count & 3)) {
			fail("event ring size/alignment");
			break;
		}
		if (count) {
			sync_dma(EVENTS, EVENT_SIZE);
			for (unsigned i = 0; i < count && !u.failed; i += 4) {
				uint32_t e = *(volatile uint32_t *)(u.dma + u.pos);

				u.pos = (u.pos + 4) % EVENT_SIZE;
				u.events++;
				event(e);
			}
			wr(GEVNTCOUNT, count);
		}
		transmit();
		uint64_t deadline;

		clock_interval_to_deadline(1, NSEC_PER_MSEC, &deadline);
		(void)assert_wait_deadline(&u.sleep_channel, THREAD_UNINT, deadline);
		thread_block(THREAD_CONTINUE_NULL);
	}
}

static void
muyu_usb_body(void)
{
	static bool attempted;
	int enabled = 0;

	PE_parse_boot_argn("muyu_usb", &enabled, sizeof(enabled));
	if (enabled != 1 || attempted) {
		muyu_usb_stage = attempted ? -2 : -1;
		muyu_usb_last = attempted ? "already attempted" : "boot-arg not seen";
		return;
	}
	attempted = true;
	muyu_usb_stage = 1;
	u.mps = 512;
	u.coding[0] = 0;
	u.coding[1] = 0xc2;
	u.coding[2] = 1;
	u.coding[6] = 8;
	printf("MUYU USB: CDC ACM debug port, console mirror and read-only commands " MUYU_TAG "\n");
	muyu_usb_stage = 2;

	/* u136: GCC's USB30 block at its ABSOLUTE address.  The device tree's GDSC nodes carry
	 * absolute reg values (0x139004, 0x150018); adding GCC's own base on top of one sends the
	 * read into unimplemented space, which stalls the single core with no fault and no panic. */
	vm_offset_t cc = ml_io_map(0x139000, 0x1000);
	uint32_t power = cc ? read_reg(cc, 0x04) : 0;

	printf("MUYU USB: usb pwr=%08x(%s,%s)\n", power,
	    (power & (1u << 31)) ? "on" : "off", (power & 1u) ? "collapse" : "expanded");
	muyu_usb_stage = 3;
	u.regs = ml_io_map(MUYU_DWC3_BASE, MUYU_DWC3_SIZE);
	u.smmu = ml_io_map(MUYU_APPS_SMMU_BASE, MUYU_APPS_SMMU_SIZE);
	if (!u.regs) {
		muyu_usb_last = "dwc3 map";
		printf("MUYU USB: DWC3 map failed\n");
		return;
	}
	uint32_t id = rd(0xc120), hw = rd(0xc140);
	/* u147: believe the field, not the earlier guess.  u144 forced 32 on the strength of a
	 * readback that was itself measured on a core sitting in reset, and the forced value then
	 * refused the ring before it was ever written. */
	unsigned width = (hw >> 24) & 0xff;

	if (width != 32 && width != 64) {
		width = 64;
	}
	printf("MUYU USB: id=%08x hwparams0=%08x awidth=%u gctl=%08x dctl=%08x dsts=%08x\n",
	    id, hw, width, rd(GCTL), rd(DCTL), rd(DSTS));
	if ((id >> 16) != 0x5533 && (id >> 16) != 0x3331) {
		muyu_usb_last = "not dwc3";
		printf("MUYU USB: not a DWC3 core; controller untouched\n");
		return;
	}
	muyu_usb_stage = 4;
	if (u.smmu) {
		printf("MUYU USB: SMMU id=%08x/%08x/%08x/%08x\n",
		    read_reg(u.smmu, 0x20), read_reg(u.smmu, 0x24),
		    read_reg(u.smmu, 0), read_reg(u.smmu, 0x28));
	}
	if (kmem_alloc_contig(kernel_map, &u.dma, DMA_SIZE, 0x3fff, 0, 0,
	    KMA_DATA | KMA_ZERO | KMA_PERMANENT, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		muyu_usb_last = "dma alloc";
		printf("MUYU USB" MUYU_TAG ": DMA allocation failed\n");
		return;
	}
	muyu_usb_dma_code = 1;
	muyu_usb_stage = 5;
	ppnum_t pn = pmap_find_phys(kernel_pmap, (addr64_t)u.dma);

	if (!pn) {
		muyu_usb_last = "dma lookup";
		printf("MUYU USB: DMA page lookup failed\n");
		return;
	}
	u.pa = ptoa_64(pn);
	if (!vusb_dma_allowed(u.pa, DMA_SIZE, width)) {
		muyu_usb_last = (u.pa & 0xfff) ? "dma align" : "dma width";
		printf("MUYU USB: DMA %llx refused (width %u)\n", u.pa, width);
		return;
	}
	for (unsigned off = 0; off < DMA_SIZE; off += PAGE_SIZE) {
		ppnum_t page = pmap_find_phys(kernel_pmap, (addr64_t)(u.dma + off));

		if (!page || ptoa_64(page) != u.pa + off) {
			muyu_usb_last = "dma not contiguous";
			printf("MUYU USB: DMA not contiguous at +%x\n", off);
			return;
		}
	}
	muyu_usb_stage = 6;
	vm_offset_t qs = ml_io_map(MUYU_QSCRATCH_BASE, MUYU_QSCRATCH_SIZE);

	if (!qs) {
		muyu_usb_last = "qscratch map";
		printf("MUYU USB: QSCRATCH map failed\n");
		return;
	}
	/* Hand-off state, as measured: ABL leaves the core stopped (DCTL.RUN = 0, DSTS.HALTED = 0)
	 * and the event ring address is ours to write.  DALEP is *not* clean - u154 read 0000000f -
	 * which is the one hand-off fact this port had been assuming away.
	 *
	 * u149 skipped the soft reset entirely because u141/u147 saw DCTL.CSFTRST never clear.  u151
	 * puts the reset back for a specific reason: those two rounds polled 20 ms at most while the
	 * guide asks for >50 ms on DWC_usb31 1.90a, and both ran with GUSB2PHYCFG.ENBLSLPM still set,
	 * which is exactly the condition the guide says can hold a command (and therefore a reset)
	 * indefinitely.  The reset now runs after the guard is applied and gets 400 ms. */
	wr(DEVTEN, 0);
	/* u155: DALEPENA is the first thing that explained the whole reset mystery.
	 *
	 * u154's table printed DALEP 0000000f - four endpoints still enabled - on a path where this
	 * file only ever writes that register after enumeration.  So the bits come from whatever
	 * programmed the controller before us, and an endpoint that is already enabled is exactly
	 * the state in which ACTION_INIT on that endpoint is accepted into the command queue and
	 * then never granted: the core has nothing to initialise.  That is the u138..u154 signature.
	 *
	 * It also explains why the soft reset "worked" in u151/u152 and nothing else did: a reset
	 * zeroes DALEPENA, so those two rounds were the only ones whose endpoints were disabled.
	 * Reinstating the reset therefore never was the fix - disabling the endpoints is.  Do that
	 * explicitly, and print what the register held so the claim is falsifiable in one boot. */
	t_dalep_in = rd(DALEPENA);
	wr(DALEPENA, 0);
	__asm__ volatile ("dsb sy" ::: "memory");
	printf("MUYU USB" MUYU_TAG ": dalep %08x -> %08x\n", t_dalep_in, rd(DALEPENA));
	unsigned i;
	uint32_t v;

	printf("MUYU USB" MUYU_TAG ": adopt dctl=%08x dsts=%08x gctl=%08x dcfg=%08x\n",
	    rd(DCTL), rd(DSTS), rd(GCTL), rd(DCFG));
	/* u151: the programming guide's rule instead of another guess.
	 *
	 * The Linux dwc3 driver wraps *every* endpoint command in this: before writing DEPCMD it
	 * clears GUSB2PHYCFG.SUSPHY (bit 6) and GUSB2PHYCFG.ENBLSLPM (bit 8), and restores them
	 * afterwards (DWC_usb3 / DWC_usb31 programming guide section 3.2.2).  With either bit set the
	 * core is allowed to hold the command until the PHY leaves suspend, and a command that waits
	 * on a PHY that never gets a token sits there with CMDACT set and CMD_STATUS 0 forever -
	 * exactly the u138/u147/u149/u151 reading.
	 *
	 * u150 cleared SUSPHY and then *set* ENBLSLPM, i.e. kept one of the two blockers in place,
	 * which is why its reading was identical to u149's.  Both are cleared here, once, and epcmd()
	 * re-applies the guard around every command.
	 *
	 * The same bits also explain the soft reset that never finished (u141/u147): a DCTL.CSFTRST
	 * that holds the PHY domain in suspend never reports completion, so the port decided the
	 * reset was broken and stopped doing it.  With the guard in place the reset is tried again
	 * with a 400 ms budget (the guide asks for >50 ms on DWC_usb31 1.90a and later) and its
	 * outcome is reported separately from the ring. */
	t_phy_in = rd(GUSB2PHYCFG(0));
	t_pipe_in = rd(GUSB3PIPECTL(0));
	printf("MUYU USB" MUYU_TAG ": adopt phy2=%08x pipe3=%08x dsts=%08x dctl=%08x\n",
	    t_phy_in, t_pipe_in, rd(DSTS), rd(DCTL));
	wr(GUSB2PHYCFG(0), t_phy_in & ~((1u << 6) | (1u << 8)));
	wr(GUSB3PIPECTL(0), t_pipe_in & ~((1u << 17) | (1u << 16)));
	__asm__ volatile ("dsb sy" ::: "memory");
	/* u154: u151 let ~60 ms pass between clearing the guard bits and running DEPCFG (the soft
	 * reset it did in between took that long); u153 gave the PHY 2 ms and DEPCFG never started.
	 * The settle is a candidate explanation in its own right, so it stays even though the reset
	 * that used to provide it is gone. */
	delay(60000);
	t_phy_cmd = rd(GUSB2PHYCFG(0));
	printf("MUYU USB" MUYU_TAG ": guarded phy2=%08x pipe3=%08x\n",
	    t_phy_cmd, rd(GUSB3PIPECTL(0)));

	/* u153: the reset is out again, on purpose.
	 *
	 * u151 carried two changes at once - this guard and a restored DCTL.CSFTRST - and it was the
	 * first round to get past DEPCFG (stage 10 -> 13).  Two rounds of readings then showed what
	 * the reset costs: GEVNTADRHI is written with 8 and reads back 0 every time afterwards, so
	 * the controller runs with no event ring at all and never enumerates.
	 *
	 * Since the guard is the part the programming guide actually asks for (section 3.2.2), it
	 * stays and the reset goes.  That also restores the one state this port has measured as
	 * healthy: u149 wrote the same ring, read 8 back, and got DEPSTARTCFG to complete.  If
	 * DEPCFG now passes as well, the guard was the whole answer; if it does not, the table at the
	 * end of this attempt says which of the two the reset was carrying. */
	printf("MUYU USB" MUYU_TAG ": halted dctl=%08x dsts=%08x\n", rd(DCTL), rd(DSTS));
	muyu_usb_stage = 8;
	sync_dma(0, DMA_SIZE);
	unsigned hi = (uint32_t)(u.pa >> 32);

	printf("MUYU USB" MUYU_TAG ": ring pa=%08x:%08x size=%x\n", hi, (uint32_t)u.pa, EVENT_SIZE);
	wr(GEVNTADRLO, (uint32_t)u.pa);
	for (i = 0; i < 3; i++) {
		wr(GEVNTADRHI, hi);
		if (rd(GEVNTADRHI) == hi) {
			break;
		}
		delay(5000);
	}
	if (rd(GEVNTADRHI) != hi) {
		/* u134: say what the core actually kept, instead of only that it disagreed.  A zero
		 * here means the register write was dropped, not that the address was rejected.
		 * u142: and keep going.  A dropped ring address used to end the round before any
		 * endpoint command ran, which is how u138's DEPCFG reading ended up being the only
		 * evidence in three consecutive builds. */
		printf("MUYU USB: event address readback: wrote %08x:%08x, read %08x:%08x\n",
		    (uint32_t)(u.pa >> 32), (uint32_t)u.pa, rd(GEVNTADRHI), rd(GEVNTADRLO));
		muyu_usb_last = "ring address rejected";
	}
	muyu_usb_ring_hi_read = (int)(rd(GEVNTADRHI) & 0x1f);
	muyu_usb_ring_ok = (rd(GEVNTADRHI) == hi) ? 1 : 0;
	wr(GEVNTSIZ, (1u << 31) | EVENT_SIZE);
	wr(GEVNTCOUNT, rd(GEVNTCOUNT) & 0xffff);
	wr(DCFG, rd(DCFG) & ~(7u | (127u << 3)));  /* high speed only */
	/* Print the state we are about to issue the first command from: after a reset the core is
	 * running again (DCTL.RUN comes back as 1) and DCFG is back at its default, so this line says
	 * what the endpoint commands will actually see. */
	printf("MUYU USB" MUYU_TAG ": ring=%08x:%08x sz=%08x dcfg=%08x dctl=%08x\n",
	    rd(GEVNTADRHI), rd(GEVNTADRLO), rd(GEVNTSIZ), rd(DCFG), rd(DCTL));
	/* u154: the order that keeps both halves.  u153 proved the ring is written and read back
	 * (HI 8, RING 1) while the core is halted; u151/u152 proved DEPCFG only completes when the
	 * core is running.  u152 lost the ring because it set RUN *before* writing it, so RUN goes
	 * here: after the address is in, before the first command. */
	if (!(rd(DCTL) & RUN)) {
		wr(DCTL, rd(DCTL) | RUN);
		for (i = 0; i < 200 && (rd(DSTS) & HALTED); i++) {
			delay(1000);
		}
		printf("MUYU USB" MUYU_TAG ": run=%s dctl=%08x dsts=%08x\n",
		    (rd(DSTS) & HALTED) ? "TIMEOUT" : "ok", rd(DCTL), rd(DSTS));
	}
	muyu_usb_stage = 9;
	if (epcmd(0, START_CONFIG, 0, 0, 0)) {
		muyu_usb_last = "ep0 start config";
		return;
	}
	muyu_usb_stage = 10;
	/* Every endpoint command runs here, with the core still halted - vermeer's order, and the
	 * point of u140.  u136's "DEPSTARTCFG completes, DEPCFG hangs" was measured with RUN
	 * already set, so the two readings were never comparable; now they share one state. */
	printf("MUYU USB" MUYU_TAG ": ep0 config dcfg=%08x dalep=%08x dctl=%08x rsc=%u\n",
	    rd(DCFG), rd(DALEPENA), rd(DCTL), u.resource[0]);
	/* u151: four parameter sets in one boot instead of four boots.  All four run with the PHY
	 * guard in place; they differ only in what the core is told about endpoint 0:
	 *   0: maxpacket 64, XferComplete|XferNotReady      - the value this port always used
	 *   1: maxpacket 512, same enables                  - Linux's SuperSpeed default for ep0
	 *   2: maxpacket 64, XferComplete only              - drop the NotReady notification
	 *   3: maxpacket 64, enables, XferRscIdx = 1        - explicit resource index in param2 */
	v = 0;
	t_cmd_pre = rd(EPBASE(0) + 12);
	/* Stop at the first accepted set: re-issuing ACTION_INIT on an endpoint that is already
	 * configured would leave it with the parameters of the *last* probe, and a wrong maxpacket
	 * on ep0 breaks enumeration more subtly than a timeout. */
	t_probe[0] = depcmd_guarded(0, SET_CONFIG, 64u << 3, 0x500, 0, 50000, &v);
	if (t_probe[0]) {
		t_probe[1] = depcmd_guarded(0, SET_CONFIG, 512u << 3, 0x500, 0, 50000, &v);
	}
	if (t_probe[0] && t_probe[1]) {
		t_probe[2] = depcmd_guarded(0, SET_CONFIG, 64u << 3, 0x100, 0, 50000, &v);
	}
	if (t_probe[0] && t_probe[1] && t_probe[2]) {
		t_probe[3] = depcmd_guarded(0, SET_CONFIG, 64u << 3, 0x500, 1u << 17, 50000, &v);
	}
	printf("MUYU USB" MUYU_TAG ": depcfg probes %d %d %d %d reg=%08x\n",
	    t_probe[0], t_probe[1], t_probe[2], t_probe[3], rd(EPBASE(0) + 12));
	if (t_probe[0] && t_probe[1] && t_probe[2] && t_probe[3]) {
		muyu_usb_last = "ep0 set config";
		usb_table();
		return;
	}
	if (!muyu_usb_ring_ok) {
		/* Endpoint commands passed but the ring address was dropped: nothing can be received
		 * without it, so save the evidence before the rest of the boot scrolls it away. */
		usb_table();
	}
	if (epcmd(0, SET_RESOURCE, 1, 0, 0)) {
		muyu_usb_last = "ep0 set resource";
		return;
	}
	if (epcmd(1, SET_CONFIG, 64u << 3, (1u << 8) | (1u << 10) | (1u << 25), 0)) {
		muyu_usb_last = "ep1 set config";
		return;
	}
	if (epcmd(1, SET_RESOURCE, 1, 0, 0)) {
		muyu_usb_last = "ep1 set resource";
		return;
	}
	wr(DALEPENA, 3);
	setup_arm();
	if (u.failed) {
		return;
	}
	muyu_usb_stage = 11;
	uint32_t hs = read_reg(qs, 0x10), ss = read_reg(qs, 0x30);

	printf("MUYU USB" MUYU_TAG ": qscratch phy2=%08x pipe=%08x\n", hs, ss);
	*(volatile uint32_t *)(qs + 0x10) = hs | (1u << 20) | (1u << 28);
	*(volatile uint32_t *)(qs + 0x30) = ss | (1u << 24);
	__asm__ volatile ("dsb sy" ::: "memory");
	wr(DEVTEN, 7 | (1u << 9) | (1u << 11));
	printf("MUYU USB" MUYU_TAG ": run from dctl=%08x dsts=%08x phy2=%08x pipe3=%08x gctl=%08x\n",
	    rd(DCTL), rd(DSTS), rd(GUSB2PHYCFG(0)), rd(GUSB3PIPECTL(0)), rd(GCTL));
	wr(DCTL, rd(DCTL) | RUN);
	for (i = 0; i < 500 && (rd(DSTS) & HALTED); i++) {
		delay(1000);
	}
	printf("MUYU USB" MUYU_TAG ": run=%s dctl=%08x dsts=%08x\n",
	    (rd(DSTS) & HALTED) ? "TIMEOUT" : "ok", rd(DCTL), rd(DSTS));
	if (rd(DSTS) & HALTED) {
		fail("controller run timeout");
		return;
	}
	muyu_usb_stage = 12;
	thread_t thread;

	if (kernel_thread_start(usb_thread, NULL, &thread) != KERN_SUCCESS) {
		fail("poll thread creation");
		wr(DCTL, rd(DCTL) & ~RUN);
		return;
	}
	thread_deallocate(thread);
	muyu_usb_stage = 13;
	printf("MUYU USB: running; DMA=%llx; serial MUYUDBG001; awaiting enumeration\n", u.pa);
}

/*
 * u139: the command engine does nothing.  u138 proved it precisely: DEPCMD reads back exactly
 * what was written (0x401 = SET_CONFIG | CMDACT), CMD_STATUS stays 0, and both attempts hang,
 * while every plain register in the core reads and writes normally and DCTL.RUN does clear
 * HALTED.  That combination - working slave interface, dead command engine and a core soft
 * reset that never completes - is the signature of a core whose *master* clock is stopped:
 * the CSR is clocked by the cfg/iface branch, the logic by the core branch.
 *
 * This port only ever read three CBCRs at offsets copied from the SM8550 reference (0x18, 0x88,
 * 0x8c) and all three happened to read 0x00000001, which proves nothing.  So dump the whole
 * USB30 clock block instead and look for branches whose CLK_OFF bit (31) is set.  Read-only:
 * a CBCR and a GDSCR share the same low bits with opposite meaning (GDSCR bit0 = collapse),
 * so nothing here is written until the map is known.
 */
static void __attribute__((unused))
muyu_clk_dump(void)
{
	vm_offset_t g = ml_io_map(0x139000, 0x200);
	vm_offset_t p = ml_io_map(0x150000, 0x100);

	if (!g) {
		printf("MUYU CLK" MUYU_TAG ": map failed g=%lx\n", (unsigned long)g);
		return;
	}
	/* u140: the full page is no longer worth 15 of the 28 lines a photo can hold.  u139's dump
	 * answered the question - power domain up (bit31 of 0x004), master branch enabled
	 * (0x018 = 00000001, divider at 0x01c), cfg/aggre at 0x060.. and 0x048 enabled, and exactly
	 * one branch parked at 0x80000000: 0x090.  So print those seven words plus one line of
	 * controller state, and spend the rest of the screen on the new sequence. */
	printf("MUYU CLK" MUYU_TAG ": pwr=%08x m18=%08x m1c=%08x c48=%08x c60=%08x a8c=%08x b90=%08x phy18=%08x\n",
	    read_reg(g, 0x04), read_reg(g, 0x18), read_reg(g, 0x1c), read_reg(g, 0x48),
	    read_reg(g, 0x60), read_reg(g, 0x8c), read_reg(g, 0x90),
	    p ? read_reg(p, 0x18) : 0);
	printf("MUYU CLK" MUYU_TAG ": gsts=%08x dctl=%08x dsts=%08x\n", rd(0xc118), rd(DCTL), rd(DSTS));
}

/*
 * u129 entry point.  Always leaves a machine-readable trace, and optionally turns it into a
 * panic so the panel stops on it: a panic line cannot scroll away, so one photo is enough.
 * Without muyu_usb_panic it prints one summary line and lets the boot continue.
 *
 * u130: only a *failure* panics.  Once the driver reports stage 13 there is nothing left to read
 * from the panel - the answer is whether the host enumerates - so the boot is allowed to continue.
 */
void
muyu_usb_debug_start(void)
{
	int panicking = 0;

	muyu_usb_body();
	PE_parse_boot_argn("muyu_usb_panic", &panicking, sizeof(panicking));
	/* u146: the clock page was answered in u139 (every branch enabled, one auxiliary off) and the
	 * dump costs fifteen lines of the only readable surface there is, so it is gone.  What is
	 * printed instead is the driver's own verdict line, and the three panel numbers are painted
	 * here as well: u144 and u145 both stopped *inside* the driver, before the heartbeat could
	 * draw anything, which is why the panel showed no digits at all. */
	muyu_vc_status_repaint();
	muyu_usb_report();
	if (panicking && muyu_usb_stage < 13) {
		panic("[u151-usb] stage=%d last=%s gctl=%08x dctl=%08x dsts=%08x",
		    muyu_usb_stage, muyu_usb_last, u.regs ? rd(GCTL) : 0, u.regs ? rd(DCTL) : 0,
		    u.regs ? rd(DSTS) : 0);
	}
}

/*
 * u130: same line, but safe to call from a timer callback (kprintf goes through the deferred
 * console ring, never through vcputc's ticket lock).  The heartbeat calls this every 15 s, so if
 * the driver claims to be running and the host still sees nothing, DSTS says whether the device
 * ever saw a connect.
 */
void
muyu_usb_report(void)
{
	kprintf("[" MUYU_BUILD_TAG "-usb stage=%d last=%s dctl=%08x dsts=%08x pa=%llx]\n",
	    muyu_usb_stage, muyu_usb_last, u.regs ? rd(DCTL) : 0, u.regs ? rd(DSTS) : 0, u.pa);
}

/*
 * u156: read the Linux side's kernel log out of pstore and paint it.
 *
 * The Linux reader has no working output of any kind yet - no UDC, so no serial port, and this
 * kernel's GKI has CONFIG_VT, CONFIG_DEVTMPFS and CONFIG_DEVMEM all off, so it cannot draw on the
 * panel or dump memory either.  What it does have is CONFIG_PSTORE_CONSOLE, and the reserved
 * ramoops region is at a fixed address in this board's DTB:
 *
 *     /reserved-memory/ramoops_region  reg = 0xa7000000 + 0x400000
 *                                      console-size = 0x200000 (offset 0)
 *                                      pmsg-size    = 0x200000 (offset 0x200000)
 *
 * So whatever the Linux kernel prints lands in DRAM at a known place, and this kernel can read
 * plain physical memory (ml_phys_read_word_64 -> phystokv, no mapping games).  Painting it with
 * muyu_vc_textdump() gives the panel a view of the other kernel's log, which is the feedback loop
 * every Linux round so far has been missing.
 */
#define PSTORE_PA          0xa7000000ULL
#define PSTORE_CONSOLE_OFF 0x0u
#define PSTORE_CONSOLE_LEN 0x200000u    /* console-size in the DTB; pmsg follows at 0x200000 */
#define PSTORE_RING_LEN    14336u
#define PSTORE_TEXT_MAX    16384u

static char muyu_pstore_txt[PSTORE_TEXT_MAX];
static char muyu_pstore_ring[PSTORE_RING_LEN];

/*
 * u156a: read the whole console zone instead of guessing which 128 KiB of it is live.
 *
 * The first version sampled the head and the tail of the 2 MiB zone and picked the denser half.
 * That is only correct if the log is shorter than one window or longer than the zone minus one
 * window; in between it reads the *oldest* text and throws the end of the boot away, which is the
 * only part worth having.
 *
 * The zone is a persistent_ram ring: a 12-byte header (sig, start, size) then characters, wrapping
 * at 2 MiB.  So: report the header verbatim - it says whether anything was ever written and how
 * much - and keep the newest 14 KiB of characters in a ring of our own.  The header line goes last
 * in the buffer because muyu_vc_textdump() paints the tail, and a summary that scrolls off the top
 * is a summary nobody can read.
 */
/*
 * u157: paint the panic-region diagnosis as one page.
 *
 * The same two lines go out on the early console from PE_create_console(), but XNU scrolls them
 * away within seconds, and by the sixth heartbeat tick the console mirror only holds the tail.
 * Painting them here is the only way they survive long enough to be photographed.
 *
 * Returns 1 when it painted (textdump freezes the panel), so the caller skips the mirror.
 */
int
muyu_pram_diag_dump(void)
{
	extern uint64_t muyu_pram_info[9];
	extern char muyu_prev_text[];
	extern int muyu_vc_textdump(const char *, unsigned) __attribute__((weak));
	static char page[1024];
	int n;

	if (muyu_vc_textdump == 0) {
		return 0;
	}
	n = snprintf(page, sizeof(page),
	    "[u157] panic region\n"
	    "base 0x%llx\n"
	    "size %llu\n"
	    "src  %s\n"
	    "prev magic 0x%08llx\n"
	    "log %llu+%llu\n"
	    "shs %llu+%llu\n"
	    "kept %llu\n",
	    (unsigned long long)muyu_pram_info[0],
	    (unsigned long long)muyu_pram_info[1],
	    muyu_pram_info[2] ? "PRAM survives reset" : "KERNEL RAM lost on reset",
	    (unsigned long long)muyu_pram_info[3],
	    (unsigned long long)muyu_pram_info[4],
	    (unsigned long long)muyu_pram_info[5],
	    (unsigned long long)muyu_pram_info[6],
	    (unsigned long long)muyu_pram_info[7],
	    (unsigned long long)muyu_pram_info[8]);
	if (n <= 0) {
		return 0;
	}
	if (muyu_pram_info[8] > 0 && (unsigned)n + 64 < sizeof(page)) {
		n += snprintf(page + n, sizeof(page) - n, "---- PREVIOUS ----\n%s",
		    muyu_prev_text);
	}
	return muyu_vc_textdump(page, (unsigned)n);
}

int
muyu_pstore_dump(void)
{
	extern int muyu_vc_textdump(const char *, unsigned) __attribute__((weak));
	uint64_t base = PSTORE_PA + PSTORE_CONSOLE_OFF;
	uint32_t sig, rstart, rsize;
	unsigned head = 0, total = 0, good = 0, first, n;

	if (muyu_vc_textdump == 0) {
		return 0;
	}
	sig = (uint32_t)ml_phys_read_word_64(base);
	rstart = (uint32_t)ml_phys_read_word_64(base + 4);
	rsize = (uint32_t)ml_phys_read_word_64(base + 8);

	for (unsigned off = 0; off < PSTORE_CONSOLE_LEN; off += 4) {
		uint32_t w = (uint32_t)ml_phys_read_word_64(base + off);

		for (unsigned k = 0; k < 4; k++) {
			unsigned char c = (unsigned char)(w >> (k * 8));

			if (c == '\n' || (c >= 32 && c < 127)) {
				if (c != '\n') {
					good++;
				}
			} else {
				c = '.';
			}
			muyu_pstore_ring[head] = (char)c;
			head = (head + 1 == PSTORE_RING_LEN) ? 0 : head + 1;
			if (total < PSTORE_RING_LEN) {
				total++;
			}
		}
	}
	/* oldest first: the ring has wrapped iff it filled up */
	first = (total == PSTORE_RING_LEN) ? head : 0;
	n = 0;
	for (unsigned i = 0; i < total && n + 64 < PSTORE_TEXT_MAX; i++) {
		muyu_pstore_txt[n++] = muyu_pstore_ring[(first + i) % PSTORE_RING_LEN];
	}
	muyu_pstore_txt[n] = '\0';
	n += snprintf(muyu_pstore_txt + n, PSTORE_TEXT_MAX - n,
	    "\nPS %08x start %u size %u text %u%s\n",
	    sig, rstart, rsize, good,
	    (good < 64) ? " EMPTY" : "");
	return muyu_vc_textdump(muyu_pstore_txt, (n < PSTORE_TEXT_MAX) ? n : PSTORE_TEXT_MAX);
}
