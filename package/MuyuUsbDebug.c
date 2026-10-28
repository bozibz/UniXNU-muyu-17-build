/* muyu (SM8635 / cliffs) CDC ACM debug console. Opt-in: muyu_usb=1.
 *
 * Adapted from ios-vermeer-lab/xnu/vermeer/vermeer_usb_debug.c (the SM8550 K70 port, hardware
 * checkpoint 2026-10-21) because this board has the same DWC3 + qcom wrapper arrangement:
 *   - DWC3 at 0xa600000 (size 0xd93c in the devicetree), QSCRATCH at <boot arg, see below>;
 *   - apps-smmu at 0x15000000, USB SID 0x40 (devicetree `iommus`);
 *   - GCC is `qcom,cliffs-gcc` at 0x100000, not SM8550's 0x139000.
 *
 * What this version deliberately does NOT do, and why:
 *   - It does not program the SMMU, the PHY, GCC clocks or PMIC: ABL already left USB in device
 *     mode for fastboot, and this driver only ever reads those blocks. The vermeer build has an
 *     exact-value identity guard for SM8550; the cliffs values are unknown, so v1 *reports* them
 *     instead of refusing. Fill the guard in once the real values are known (see the `dbg` output).
 *   - It does not write the Qualcomm VBUS-valid override, because the QSCRATCH base on cliffs is
 *     unconfirmed. If enumeration never happens, retry with muyu-usb-qscratch=0xa6f8800.
 *
 * Log source: XNU's own msgbuf ring, not console hooks. That drops two of the four patch points the
 * vermeer integration needed (_cnputs and serial_putc_crlf): everything printf/IOLog writes is
 * already in msgbuf, and a kernel this late has nothing interesting before msgbuf exists.
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/msgbuf.h>
#include <libkern/libkern.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <mach/thread_act.h>
#include <kern/sched_prim.h>
#include <machine/machine_routines.h>
#include <arm/caches_internal.h>
#include <vm/pmap.h>
#include <vm/vm_kern.h>
#include <pexpert/pexpert.h>
#include "MuyuUsbProtocol.h"

extern vm_offset_t ml_io_map(vm_offset_t, vm_size_t);
extern ppnum_t pmap_find_phys(pmap_t, addr64_t);
extern void IOSleep(unsigned milliseconds);   /* not declared by the headers the kernel record pulls in */

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
#define EPBASE(n) (0xc800+16*(n))
#define RUN (1u<<31)
#define RESET (1u<<30)
#define HALTED (1u<<22)
#define ACTIVE (1u<<10)
#define FORCE_REMOVE (1u<<11)
#define SET_CONFIG 1
#define SET_RESOURCE 2
#define STALL 4
#define CLEAR_STALL 5
#define START 6
#define END 8
#define START_CONFIG 9
#define T_NORMAL (1u<<4)
#define T_SETUP (2u<<4)
#define T_STATUS2 (3u<<4)
#define T_STATUS3 (4u<<4)
#define T_DATA (5u<<4)
#define HWO 1u
#define TRB_FLAGS (HWO|2u|(1u<<10)|(1u<<11))
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

#define MUYU_USB_GCC_BASE 0x100000u
#define MUYU_USB_DWC3_BASE 0xa600000u
#define MUYU_USB_DWC3_SIZE 0xe000u
#define MUYU_USB_SMMU_BASE 0x15000000u
/* GCC (qcom,cliffs-gcc) USB30 primary block. Offsets from the mainline gcc-sm8650 driver, which is the
 * same IP family: usb30_prim_gdsc 0x39004, usb3_phy_gdsc 0x50018, branch clocks 0x39018/24/28/60/64/68/88/8c.
 * Only the enable bits are written; rates and parents are left exactly as ABL programmed them. */
#define MUYU_GCC_USB30_GDSC 0x39004u
#define MUYU_GCC_USB3PHY_GDSC 0x50018u
static const uint32_t gcc_usb_branches[]={0x39018,0x39024,0x39028,0x39060,0x39064,0x39068,0x39088,0x3908c};
static const uint32_t gcc_usb_probe[]={0x39004,0x39018,0x39024,0x39028,0x3902c,0x39060,0x39068,0x39088,0x3908c,0x50018};
#define MUYU_USB_SMMU_SIZE 0x100000u

static const unsigned trb_off[6]={0x1000,0x1040,0x1080,0x10c0,0x1100,0x1140};
static const char *const usb_strings[]={NULL,"MUYU Lab","MUYU XNU Debug","MUYUDBG001"};
static struct {
    vm_offset_t regs, dma, smmu;
    uint64_t pa, tx_bytes, rx_bytes;
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
    /* Diagnostics: captured once at start so `dbg` and the dump line can report them even when the
     * controller is left untouched. */
    uint32_t gcc[4], id, hwparams, awidth, gctl, dctl, dsts;
    uint32_t smmu_base[4], route, cbar, sctlr;
    int smmu_matches, smmu_index;
    uint32_t qs[2];
    uint32_t qscratch_base;
    const char *stage;
    const char *why;
    bool dma_low;
    uint32_t reset_wb, csf_wb;
    uint32_t gcc_before[10], gcc_after[10];
    int gdsc30, gdscphy;
    uint32_t step_rc[8];
    int replay_pending;
} u;

/* msgbuf streaming cursor. -1 means "not attached yet". */
static int g_msg_lastx = -1;

static uint32_t read_reg(vm_offset_t base,unsigned off)
{
    uint32_t v=*(volatile uint32_t *)(base+off);
    __asm__ volatile("dmb oshld" ::: "memory");
    return v;
}
static uint32_t rd(unsigned off) { return read_reg(u.regs,off); }
static void wr(unsigned off,uint32_t v)
{
    *(volatile uint32_t *)(u.regs+off)=v;
    __asm__ volatile("dsb sy" ::: "memory");
}
static void sync_dma(unsigned off,unsigned size)
{
    flush_dcache64((addr64_t)(u.pa+off),size,TRUE);
    __asm__ volatile("dsb sy" ::: "memory");
}
static void fail(const char *why)
{
    if (!u.failed) printf("MUYU USB: stopped: %s; permanent DMA retained\n",why);
    if (!u.why) u.why=why;
    u.failed=true; u.stage="stopped";
}
static int epcmd(unsigned ep,unsigned cmd,uint32_t p0,uint32_t p1,uint32_t p2)
{
    if (u.failed || ep>=6) return -1;
    wr(EPBASE(ep)+8,p0); wr(EPBASE(ep)+4,p1); wr(EPBASE(ep),p2);
    wr(EPBASE(ep)+12,cmd|ACTIVE);
    for (unsigned n=0;n<1000;n++) {
        uint32_t v=rd(EPBASE(ep)+12);
        if (!(v&ACTIVE)) {
            if ((v>>12)&15) { fail("endpoint command status"); return -1; }
            if ((cmd&15)==START) u.resource[ep]=(v>>16)&0x7f;
            return 0;
        }
        delay(1);
    }
    fail("endpoint command timeout"); return -1;
}
static void transfer(unsigned ep,unsigned buf,unsigned length,unsigned kind)
{
    if (u.failed) return;
    if (ep>=6 || u.busy[ep] || buf>DMA_SIZE || length>DMA_SIZE-buf) {
        fail("transfer ownership/bounds"); return;
    }
    unsigned off=trb_off[ep];
    uint64_t p=u.pa+buf,t=u.pa+off;
    volatile uint32_t *trb=(volatile uint32_t *)(u.dma+off);
    trb[0]=(uint32_t)p; trb[1]=(uint32_t)(p>>32);
    trb[2]=length; trb[3]=kind|TRB_FLAGS;
    u.requested[ep]=length;
    if (length) sync_dma(buf,length);
    sync_dma(off,16);
    u.busy[ep]=true;
    (void)epcmd(ep,START,(uint32_t)(t>>32),(uint32_t)t,0);
}
static int complete(unsigned ep)
{
    if (ep>=6 || !u.busy[ep]) { fail("unexpected completion"); return -1; }
    sync_dma(trb_off[ep],16);
    volatile uint32_t *t=(volatile uint32_t *)(u.dma+trb_off[ep]);
    unsigned left=t[2]&0xffffff;
    if ((t[3]&HWO) || (t[2]>>28) || left>u.requested[ep]) {
        fail("invalid DMA completion"); return -1;
    }
    u.busy[ep]=false;
    return (int)(u.requested[ep]-left);
}
static void setup_arm(void)
{
    u.phase=SETUP_PHASE;
    transfer(0,SETUP,8,T_SETUP);
}
static void stall(void)
{
    (void)epcmd(0,STALL,0,0,0);
    setup_arm();
}
static void receive_arm(void) { transfer(2,RX,RX_SIZE,T_NORMAL); }
static void stop_data(void)
{
    for (unsigned ep=2;ep<6;ep++) {
        if (u.busy[ep]) {
            if (!u.resource[ep] || epcmd(ep,END|FORCE_REMOVE|(u.resource[ep]<<16),0,0,0)) return;
            u.busy[ep]=false;
        }
    }
    wr(DALEPENA,3);
    u.configured=false; u.dtr=false; u.notify=false;
}
static void configure(void)
{
    if (u.configured || u.failed) return;
    if (epcmd(0,START_CONFIG|(2u<<16),0,0,0) ||
        epcmd(2,SET_CONFIG,(2u<<1)|(u.mps<<3),(1u<<8)|(2u<<25),0) ||
        epcmd(2,SET_RESOURCE,1,0,0) ||
        epcmd(3,SET_CONFIG,(2u<<1)|(u.mps<<3)|(1u<<17),(1u<<8)|(3u<<25),0) ||
        epcmd(3,SET_RESOURCE,1,0,0) ||
        epcmd(5,SET_CONFIG,(3u<<1)|(16u<<3)|(2u<<17),(1u<<8)|(8u<<16)|(5u<<25),0) ||
        epcmd(5,SET_RESOURCE,1,0,0)) return;
    wr(DALEPENA,3|(1u<<2)|(1u<<3)|(1u<<5));
    u.configured=true; receive_arm();
    printf("MUYU USB: CDC ACM configured, bulk MPS %u\n",u.mps);
}
static unsigned string_desc(unsigned index,uint8_t *data)
{
    if (!index) { data[0]=4;data[1]=3;data[2]=9;data[3]=4; return 4; }
    if (index>=sizeof(usb_strings)/sizeof(usb_strings[0])) return 0;
    unsigned n=2;
    for (const char *s=usb_strings[index];*s && n<126;s++) { data[n++]=(uint8_t)*s;data[n++]=0; }
    data[0]=(uint8_t)n;data[1]=3;return n;
}

/* msgbuf access. The ring is read with a wrap-aware cursor so live streaming never repeats a line;
 * `replay` instead sends the newest tail once. */
static int msg_live(char *out,unsigned cap)
{
    struct msgbuf *m=msgbufp;
    if (!m || !m->msg_bufc || m->msg_size<=1 || m->msg_magic!=MSG_MAGIC) return 0;
    int x=m->msg_bufx;
    if (x<0 || x>m->msg_size) x=0;
    if (g_msg_lastx<0) { g_msg_lastx=x; u.stage="streaming"; return 0; }
    unsigned n=0;
    while (g_msg_lastx!=x && n<cap) {
        out[n++]=((const char *)m->msg_bufc)[g_msg_lastx];
        if (++g_msg_lastx>=(int)m->msg_size) g_msg_lastx=0;
    }
    return (int)n;
}
static int msg_tail(char *out,unsigned cap)
{
    struct msgbuf *m=msgbufp;
    if (!m || !m->msg_bufc || m->msg_size<=1 || m->msg_magic!=MSG_MAGIC) return 0;
    unsigned size=(unsigned)m->msg_size;
    int x=m->msg_bufx;
    if (x<0 || (unsigned)x>size) x=0;
    unsigned n=cap<size?cap:size;
    unsigned start=((unsigned)x+size-n)%size;
    for (unsigned i=0;i<n;i++) out[i]=((const char *)m->msg_bufc)[(start+i)%size];
    return (int)n;
}
static void handle_setup(void)
{
    sync_dma(SETUP,8);
    const uint8_t *s=(const uint8_t *)(u.dma+SETUP);
    uint8_t *data=(uint8_t *)(u.dma+DATA);
    unsigned value=s[2]|(s[3]<<8),index=s[4]|(s[5]<<8),length=s[6]|(s[7]<<8),n=0;
    int kind=vusb_request(s[0],s[1],value,index,length);
    u.data_in=(s[0]&0x80)!=0;u.set_line=false;
    if (u.setups++<40) printf("MUYU USB: setup %02x %02x %04x %04x len%u\n",s[0],s[1],value,index,length);
    switch (kind) {
    case VUSB_DESC:
        switch (value>>8) {
        case 1: if (value&255) break;memcpy(data,vusb_device,sizeof(vusb_device));n=sizeof(vusb_device);break;
        case 2: case 7:
            if (value&255) break;
            memcpy(data,vusb_config,sizeof(vusb_config));n=sizeof(vusb_config);
            data[1]=(uint8_t)(value>>8);
            /* Configuration and other-speed configuration have opposite MPS. */
            for (unsigned i=0;i<n;i+=data[i]) if (data[i+1]==5 && (data[i+3]&3)==2) {
                unsigned m=(value>>8)==7?(u.mps==512?64:512):u.mps;
                data[i+4]=(uint8_t)m;data[i+5]=(uint8_t)(m>>8);
            }
            break;
        case 3:n=string_desc(value&255,data);break;
        case 6:if (!(value&255)) { memcpy(data,vusb_qualifier,sizeof(vusb_qualifier));n=sizeof(vusb_qualifier); }break;
        default:break;
        }
        if (!n) { stall();return; }break;
    case VUSB_ADDRESS:wr(DCFG,(rd(DCFG)&~(127u<<3))|(value<<3));break;
    case VUSB_CONFIG:
        if (!value && u.configured) { fail("host deconfigured; reboot to reconnect"); return; }
        if (value) configure();break;
    case VUSB_GET_CONFIG:data[0]=u.configured?1:0;n=1;break;
    case VUSB_STATUS:data[0]=data[1]=0;n=2;break;
    case VUSB_INTERFACE:break;
    case VUSB_GET_INTERFACE:data[0]=0;n=1;break;
    case VUSB_SET_LINE:u.set_line=true;break;
    case VUSB_GET_LINE:memcpy(data,u.coding,7);n=7;break;
    case VUSB_CONTROL:
        if (u.configured) {
            bool old=u.dtr;u.dtr=(value&1)!=0;u.notify=true;
            if (u.dtr && !old) printf("MUYU USB: debug connected; help/status/ping/replay/dbg (read only)\n");
        }break;
    case VUSB_BREAK:break;
    case VUSB_CLEAR_HALT:
        if (u.busy[(index&15)*2+((index>>7)&1)]) { fail("clear-halt on active endpoint");return; }
        (void)epcmd((index&15)*2+((index>>7)&1),CLEAR_STALL,0,0,0);break;
    default:stall();return;
    }
    if (u.failed) return;
    if (!length) { u.phase=WAIT2;return; }
    u.phase=DATA_PHASE;
    if (u.data_in) transfer(1,DATA,n<length?n:length,T_DATA);
    else if (u.set_line) transfer(0,DATA,64,T_DATA);
    else stall();
}
static void command_line(void)
{
    u.command[u.command_n]=0;
    if (u.command_overflow) printf("MUYU DEBUG: command too long\n");
    else if (!strcmp(u.command,"ping")) printf("MUYU DEBUG: pong MUYUDBG001\n");
    else if (!strcmp(u.command,"status"))
        printf("MUYU DEBUG: MUYUDBG001 events=%u tx=%llu rx=%llu stage=%s failed=%d\n",
            u.events,u.tx_bytes,u.rx_bytes,u.stage?u.stage:"?",(int)u.failed);
    else if (!strcmp(u.command,"replay")) {
        u.replay_pending=1;printf("MUYU DEBUG: replay retained msgbuf tail\n");
    } else if (!strcmp(u.command,"dbg"))
        printf("MUYU DEBUG: gcc=%08x/%08x/%08x/%08x dwc3 id=%08x hw=%08x aw=%u gctl=%08x dctl=%08x dsts=%08x\n"
               "MUYU DEBUG: smmu base=%08x/%08x/%08x/%08x sid40 matches=%d index=%d route=%08x cbar=%08x sctlr=%08x qs=%08x/%08x\n",
            u.gcc[0],u.gcc[1],u.gcc[2],u.gcc[3],u.id,u.hwparams,u.awidth,u.gctl,u.dctl,u.dsts,
            u.smmu_base[0],u.smmu_base[1],u.smmu_base[2],u.smmu_base[3],
            u.smmu_matches,u.smmu_index,u.route,u.cbar,u.sctlr,u.qs[0],u.qs[1]);
    else if (u.command_n) printf("MUYU DEBUG: commands help status ping replay dbg; queries only\n");
    u.command_n=0;u.command_overflow=false;
}
static void receive_done(unsigned length)
{
    sync_dma(RX,RX_SIZE);u.rx_bytes+=length;
    const uint8_t *p=(const uint8_t *)(u.dma+RX);
    for (unsigned i=0;i<length;i++) {
        if (p[i]=='\r' || p[i]=='\n') command_line();
        else if (p[i]>=32 && p[i]<=126) {
            if (u.command_n<sizeof(u.command)-1) u.command[u.command_n++]=(char)p[i];
            else u.command_overflow=true;
        }
    }
    receive_arm();
}
static void transmit(void)
{
    if (!u.configured || !u.dtr || u.failed) return;
    if (u.notify && !u.busy[5]) {
        const uint8_t msg[10]={0xa1,0x20,0,0,0,0,2,0,3,0};
        memcpy((void *)(u.dma+NOTIFY),msg,sizeof(msg));u.notify=false;
        transfer(5,NOTIFY,sizeof(msg),T_NORMAL);
    }
    if (u.busy[3]) return;
    unsigned n;
    if (u.replay_pending) {
        u.replay_pending=0;
        n=(unsigned)msg_tail((char *)(u.dma+TX),TX_SIZE);
    } else {
        n=(unsigned)msg_live((char *)(u.dma+TX),TX_SIZE);
    }
    /* Serial stream must have a short packet, with no padding/invented bytes. */
    if (n && !(n%u.mps)) n--;
    if (n) { transfer(3,TX,n,T_NORMAL);u.tx_bytes+=n; }
}
static void event(uint32_t e)
{
    if (e&1) {
        if ((e&0xfe)!=0) { fail("unexpected device event format");return; }
        unsigned type=(e>>8)&15;
        if (type==1) {
            if (u.configured) { stop_data();fail("bus reset after configuration; reboot to reconnect");return; }
            wr(DCFG,rd(DCFG)&~(127u<<3));printf("MUYU USB: bus reset\n");
        } else if (type==2) {
            unsigned speed=rd(DSTS)&7;
            if (speed!=0 && speed!=1 && speed!=3) { fail("unexpected non-USB2 speed");return; }
            u.mps=speed==0?512:64;
            (void)epcmd(0,SET_CONFIG,(64u<<3)|(2u<<30),(1u<<8)|(1u<<10),0);
            (void)epcmd(1,SET_CONFIG,(64u<<3)|(2u<<30),(1u<<8)|(1u<<10)|(1u<<25),0);
            printf("MUYU USB: connected speed=%u\n",speed);
        } else if (!type && u.configured) fail("cable disconnect; reboot to reconnect");
        else if (type==9 || type==11) fail("controller/event overflow");
        return;
    }
    unsigned ep=(e>>1)&31,type=(e>>6)&15,status=(e>>12)&15;
    if (type==1) {
        int n=complete(ep);if (n<0) return;
        if (status&1) { fail("USB transfer bus error");return; }
        if (ep==2) { receive_done((unsigned)n);return; }
        if (ep>1) return;
        switch (u.phase) {
        case SETUP_PHASE:if (ep!=0 || n!=8) fail("setup length");else handle_setup();break;
        case DATA_PHASE:
            if (u.set_line) {
                if (ep!=0 || n!=7) { fail("line coding length");break; }
                sync_dma(DATA,64);memcpy(u.coding,(void *)(u.dma+DATA),7);
            }
            u.phase=WAIT3;break;
        case STATUS_PHASE:setup_arm();break;
        default:fail("control completion order");break;
        }
    } else if (ep<=1 && type==3 && status==2 && (u.phase==WAIT2 || u.phase==WAIT3)) {
        unsigned direction=u.phase==WAIT2?1:(u.data_in?0:1);
        if (ep!=direction) { fail("control status direction");return; }
        unsigned kind=u.phase==WAIT3?T_STATUS3:T_STATUS2;
        u.phase=STATUS_PHASE;transfer(ep,DATA,0,kind);
    }
}
static void usb_thread(void *arg __unused,wait_result_t result __unused)
{
    while (!u.failed) {
        unsigned count=rd(GEVNTCOUNT)&0xffff;
        if (count>EVENT_SIZE || (count&3)) { fail("event ring size/alignment");break; }
        if (count) {
            sync_dma(EVENTS,EVENT_SIZE);
            for (unsigned i=0;i<count && !u.failed;i+=4) {
                uint32_t e=*(volatile uint32_t *)(u.dma+u.pos);
                u.pos=(u.pos+4)%EVENT_SIZE;u.events++;
                event(e);
            }
            wr(GEVNTCOUNT,count);
        }
        transmit();
        uint64_t deadline;clock_interval_to_deadline(1,NSEC_PER_MSEC,&deadline);
        (void)assert_wait_deadline(&u.sleep_channel,THREAD_UNINT,deadline);
        thread_block(THREAD_CONTINUE_NULL);
    }
    wr(DEVTEN,0);wr(DCTL,rd(DCTL)&~RUN);
    thread_terminate(current_thread());
}

/* Bring the USB clock/power blocks back up. ABL used them for fastboot and then took them away, which
 * leaves the DWC3 readable over the APB interface but unable to complete a device reset. Only enable bits
 * are touched: clearing GDSCR.SW_COLLAPSE and setting each branch clock's enable bit. Returns 0 when the
 * two power domains never report POWER_ON, so the caller can say so instead of guessing. */
static int gcc_usb_up(vm_offset_t gcc)
{
    for (unsigned i=0;i<10;i++) u.gcc_before[i]=read_reg(gcc,gcc_usb_probe[i]);
    u.gdsc30=-1; u.gdscphy=-1;
    if (!gcc) return -1;
    uint32_t v=read_reg(gcc,MUYU_GCC_USB30_GDSC);
    if (v&1u) { *(volatile uint32_t *)(gcc+MUYU_GCC_USB30_GDSC)=v&~1u; __asm__ volatile("dsb sy" ::: "memory"); }
    for (unsigned i=0;i<500 && !(read_reg(gcc,MUYU_GCC_USB30_GDSC)&(1u<<31));i++) delay(1000);
    u.gdsc30=(read_reg(gcc,MUYU_GCC_USB30_GDSC)>>31)&1;
    v=read_reg(gcc,MUYU_GCC_USB3PHY_GDSC);
    if (v&1u) { *(volatile uint32_t *)(gcc+MUYU_GCC_USB3PHY_GDSC)=v&~1u; __asm__ volatile("dsb sy" ::: "memory"); }
    for (unsigned i=0;i<500 && !(read_reg(gcc,MUYU_GCC_USB3PHY_GDSC)&(1u<<31));i++) delay(1000);
    u.gdscphy=(read_reg(gcc,MUYU_GCC_USB3PHY_GDSC)>>31)&1;
    for (unsigned i=0;i<sizeof(gcc_usb_branches)/sizeof(gcc_usb_branches[0]);i++) {
        uint32_t b=read_reg(gcc,gcc_usb_branches[i]);
        if (!(b&1u)) *(volatile uint32_t *)(gcc+gcc_usb_branches[i])=b|1u;
    }
    __asm__ volatile("dsb sy" ::: "memory");
    for (unsigned i=0;i<100;i++) delay(1000);
    for (unsigned i=0;i<10;i++) u.gcc_after[i]=read_reg(gcc,gcc_usb_probe[i]);
    return 0;
}

/* Read-only survey of the SMMU: finds the stream matching SID 0x40 and its route, exactly the values
 * the vermeer guard compares against. Reported, never enforced, until the cliffs values are known. */
static void smmu_survey(void)
{
    vm_offset_t s=u.smmu;
    u.smmu_matches=0; u.smmu_index=-1; u.route=0; u.cbar=0; u.sctlr=1;
    u.smmu_base[0]=read_reg(s,0x20); u.smmu_base[1]=read_reg(s,0x24);
    u.smmu_base[2]=read_reg(s,0);    u.smmu_base[3]=read_reg(s,0x28);
    for (unsigned i=0;i<105;i++) {
        uint32_t smr=read_reg(s,0x800+4*i);
        if ((smr>>31) && !((smr^0x40)&~((smr>>16)&0x7fff)&0xffff)) {
            u.smmu_matches++; u.smmu_index=(int)i; u.route=read_reg(s,0xc00+4*i);
        }
    }
    unsigned cb=u.route&255;
    if (((u.route>>16)&3)==0 && cb<70) {
        u.cbar=read_reg(s,0x1000+4*cb);
        u.sctlr=read_reg(s,0x80000+0x1000*cb);
    }
}

static void report(void)
{
    printf("MUYU USB: gcc=%08x/%08x/%08x/%08x dwc3 id=%08x hwparams=%08x awidth=%u\n",
        u.gcc[0],u.gcc[1],u.gcc[2],u.gcc[3],u.id,u.hwparams,u.awidth);
    printf("MUYU USB: gctl=%08x dctl=%08x dsts=%08x gdsc30=%d gdscphy=%d\n",u.gctl,u.dctl,u.dsts,u.gdsc30,u.gdscphy);
    printf("MUYU USB: gcc_usb before %08x %08x %08x %08x %08x %08x\n",
        u.gcc_before[0],u.gcc_before[1],u.gcc_before[2],u.gcc_before[3],u.gcc_before[9],u.gcc_before[5]);
    printf("MUYU USB: gcc_usb after  %08x %08x %08x %08x %08x %08x\n",
        u.gcc_after[0],u.gcc_after[1],u.gcc_after[2],u.gcc_after[3],u.gcc_after[9],u.gcc_after[5]);
    printf("MUYU USB: smmu=%08x/%08x/%08x/%08x sid40 matches=%d index=%d route=%08x cbar=%08x sctlr=%08x\n",
        u.smmu_base[0],u.smmu_base[1],u.smmu_base[2],u.smmu_base[3],
        u.smmu_matches,u.smmu_index,u.route,u.cbar,u.sctlr);
}

/* One line for the msgbuf dump page / panic text, so a failed probe is still readable from a photo
 * when this very USB channel is the thing that failed. */
void muyu_usb_state_line(char *out,size_t cap)
{
    if (!out || cap<32) return;
    snprintf(out,cap,"USB st=%s failed=%d why=%s ev=%u tx=%llu gctl=%08x dsts=%08x smmu40=%d route=%08x\n",
        u.stage?u.stage:"off",(int)u.failed,u.why?u.why:"none",u.events,u.tx_bytes,u.gctl,u.dsts,
        u.smmu_matches,u.route);
}

/* muyu-usb-panic-at=<seconds>: guaranteed readable readout of the survey when enumeration does not
 * happen. The panel dump is useless after the graphics client starts (see HANDOFF-2026-10-28 §13);
 * panic text is not. */
static void panic_thread(void *arg,wait_result_t result __unused)
{
    uint32_t at=(uint32_t)(uintptr_t)arg;
    IOSleep(at*1000);
    panic("MUYU USB survey: stage=%s failed=%d why=%s\n"
          "gcc=%08x/%08x/%08x/%08x\n"
          "dwc3 id=%08x hwparams=%08x awidth=%u gctl=%08x dctl=%08x dsts=%08x\n"
          "smmu=%08x/%08x/%08x/%08x matches=%d index=%d route=%08x cbar=%08x sctlr=%08x qs=%08x/%08x\n"
          "gdsc30=%d gdscphy=%d gdscr=%08x phygdscr=%08x br18=%08x br60=%08x br88=%08x br8c=%08x\n"
          "dma=%llx low=%d reset_wb=%08x csf_wb=%08x ev=%u setups=%u tx=%llu rx=%llu ep0=%x/%x/%x/%x/%x\n",
        u.stage?u.stage:"off",(int)u.failed,u.why?u.why:"none",
        u.gcc[0],u.gcc[1],u.gcc[2],u.gcc[3],
        u.id,u.hwparams,u.awidth,u.gctl,u.dctl,u.dsts,
        u.smmu_base[0],u.smmu_base[1],u.smmu_base[2],u.smmu_base[3],
        u.smmu_matches,u.smmu_index,u.route,u.cbar,u.sctlr,u.qs[0],u.qs[1],
        u.gdsc30,u.gdscphy,u.gcc_after[0],u.gcc_after[9],
        u.gcc_after[1],u.gcc_after[5],u.gcc_after[8],u.gcc_after[7],
        u.pa,(int)u.dma_low,u.reset_wb,u.csf_wb,u.events,u.setups,u.tx_bytes,u.rx_bytes,
        u.step_rc[0],u.step_rc[1],u.step_rc[2],u.step_rc[3],u.step_rc[4]);
}

static void step(unsigned i,int rc) { if (i<8 && u.step_rc[i]==0 && rc) u.step_rc[i]=(uint32_t)rc; }

void muyu_usb_debug_start(void)
{
    static bool attempted;
    int enabled=0;
    u.stage="off";
    if (!PE_parse_boot_argn("muyu_usb",&enabled,sizeof(enabled)) || enabled!=1) return;
    if (attempted) return;
    attempted=true;
    u.stage="start";
    /* Arm the readable readout first: every early return below would otherwise leave nothing to look
     * at, and this driver is exactly the case where the channel under test is the one that failed. */
    {
        uint32_t at=0;
        thread_t reporter;
        if (PE_parse_boot_argn("muyu-usb-panic-at",&at,sizeof(at)) && at>=5 &&
            kernel_thread_start(panic_thread,(void *)(uintptr_t)at,&reporter)==KERN_SUCCESS)
            thread_deallocate(reporter);
    }
    u.mps=512;u.coding[0]=0;u.coding[1]=0xc2;u.coding[2]=1;u.coding[6]=8;
    printf("MUYU USB: CDC ACM debug001, msgbuf log and read-only commands\n");

    unsigned qs_arg=0;
    if (PE_parse_boot_argn("muyu-usb-qscratch",&qs_arg,sizeof(qs_arg))) u.qscratch_base=qs_arg;

    /* GCC: read-only. SM8550's offsets are kept as a first look because cliffs is a different IP
     * block; the printed values are what the real guard will be written from. */
    /* The USB block lives at 0x39000 inside GCC and the PHY power domain at 0x50018, so a single page is
     * not enough: mapping 0x1000 and touching 0x39004 is a data abort (measured 2026-10-28 19:22). */
    vm_offset_t gcc=ml_io_map(MUYU_USB_GCC_BASE,0x100000);
    if (gcc) {
        u.gcc[0]=read_reg(gcc,4);u.gcc[1]=read_reg(gcc,0x18);
        u.gcc[2]=read_reg(gcc,0x88);u.gcc[3]=read_reg(gcc,0x8c);
    }

    u.regs=ml_io_map(MUYU_USB_DWC3_BASE,MUYU_USB_DWC3_SIZE);
    u.smmu=ml_io_map(MUYU_USB_SMMU_BASE,MUYU_USB_SMMU_SIZE);
    if (!u.regs || !u.smmu) { u.stage="map"; printf("MUYU USB: register map failed\n"); return; }
    u.id=rd(0xc120);u.hwparams=rd(0xc140);u.awidth=u.hwparams>>24;
    u.gctl=rd(GCTL);u.dctl=rd(DCTL);u.dsts=rd(DSTS);
    smmu_survey();
    gcc_usb_up(gcc);
    report();
    if ((u.id>>16)!=0x5533 && (u.id>>16)!=0x3331) {
        u.stage="bad-id"; printf("MUYU USB: unexpected controller id; untouched\n"); return;
    }
    if (((u.gctl>>12)&3)!=2) {
        u.stage="host-mode"; printf("MUYU USB: controller not in device mode; untouched\n"); return;
    }

    /* The controller can only reach part of DRAM. Ask for the low 4 GiB first (max_pnum is in pages), which
     * is where the DWC3's addresses are always valid, and fall back to unconstrained only if that fails. */
    kern_return_t kr=kmem_alloc_contig(kernel_map,&u.dma,DMA_SIZE,PAGE_MASK,
        (ppnum_t)0x100000,0,KMA_DATA|KMA_ZERO|KMA_PERMANENT,VM_KERN_MEMORY_IOKIT);
    u.dma_low=(kr==KERN_SUCCESS);
    if (kr!=KERN_SUCCESS)
        kr=kmem_alloc_contig(kernel_map,&u.dma,DMA_SIZE,PAGE_MASK,0,0,
            KMA_DATA|KMA_ZERO|KMA_PERMANENT,VM_KERN_MEMORY_IOKIT);
    if (kr!=KERN_SUCCESS) {
        u.stage="dma-alloc";printf("MUYU USB: contiguous DMA allocation failed\n");return;
    }
    ppnum_t pn=pmap_find_phys(kernel_pmap,(addr64_t)u.dma);
    if (!pn) { u.stage="dma-phys";printf("MUYU USB: DMA not physically mapped\n");return; }
    u.pa=ptoa_64(pn);
    for (unsigned off=0;off<DMA_SIZE;off+=PAGE_SIZE) {
        ppnum_t page=pmap_find_phys(kernel_pmap,(addr64_t)(u.dma+off));
        if (!page || ptoa_64(page)!=u.pa+off) { u.stage="dma-contig";printf("MUYU USB: DMA not contiguous\n");return; }
    }
    printf("MUYU USB: DMA pa=%llx (%u bytes)\n",u.pa,DMA_SIZE);

    /* Optional Qualcomm wrapper VBUS-valid override. Base unconfirmed on cliffs, so only when asked. */
    if (u.qscratch_base) {
        vm_offset_t qs=ml_io_map(u.qscratch_base,0x100);
        if (qs) {
            u.qs[0]=read_reg(qs,0x10);u.qs[1]=read_reg(qs,0x30);
            *(volatile uint32_t *)(qs+0x10)=u.qs[0]|(1u<<20)|(1u<<28);
            *(volatile uint32_t *)(qs+0x30)=u.qs[1]|(1u<<24);
            __asm__ volatile("dsb sy" ::: "memory");
            printf("MUYU USB: qscratch @%08x was %08x/%08x\n",u.qscratch_base,u.qs[0],u.qs[1]);
        }
    }

    u.stage="reset";
    wr(DEVTEN,0);wr(DCTL,rd(DCTL)&~RUN);
    unsigned i;
    for (i=0;i<500 && !(rd(DSTS)&HALTED);i++) delay(1000);
    if (!(rd(DSTS)&HALTED)) { fail("controller halt timeout");return; }
    /* Two questions in one: does the write land at all (read back immediately), and does the controller
     * ever clear it. A write that does not land means the core clock is gated, which only GCC can undo;
     * a write that lands but never clears means the core is held in reset somewhere else. */
    wr(DCTL,RESET);
    u.reset_wb=rd(DCTL);
    for (i=0;i<5000 && (rd(DCTL)&RESET);i++) delay(1000);
    if (rd(DCTL)&RESET) {
        /* DWC3 also has a core soft reset in GCTL. Record what that one does before giving up. */
        uint32_t g=rd(GCTL);
        wr(GCTL,g|(1u<<10));
        u.csf_wb=rd(GCTL);
        for (i=0;i<5000 && (rd(GCTL)&(1u<<10));i++) delay(1000);
        fail(rd(GCTL)&(1u<<10)?"controller reset timeout (DCTL and GCTL.CSFTRST)":"DCTL reset timeout, CSFTRST ok");
        return;
    }
    sync_dma(0,DMA_SIZE);
    wr(GEVNTADRLO,(uint32_t)u.pa);wr(GEVNTADRHI,(uint32_t)(u.pa>>32));
    if (rd(GEVNTADRHI)!=(uint32_t)(u.pa>>32)) { fail("event address readback");return; }
    wr(GEVNTSIZ,(1u<<31)|EVENT_SIZE);wr(GEVNTCOUNT,rd(GEVNTCOUNT)&0xffff);
    wr(DCFG,rd(DCFG)&~(7u|(127u<<3))); /* high speed only */
    u.stage="ep0";
    step(0,epcmd(0,START_CONFIG,0,0,0));
    step(1,epcmd(0,SET_CONFIG,64u<<3,(1u<<8)|(1u<<10),0));
    step(2,epcmd(0,SET_RESOURCE,1,0,0));
    step(3,epcmd(1,SET_CONFIG,64u<<3,(1u<<8)|(1u<<10)|(1u<<25),0));
    step(4,epcmd(1,SET_RESOURCE,1,0,0));
    if (u.failed) return;
    wr(DALEPENA,3);setup_arm();if (u.failed) return;
    u.stage="run";
    wr(DEVTEN,7|(1u<<9)|(1u<<11));wr(DCTL,rd(DCTL)|RUN);
    for (i=0;i<500 && (rd(DSTS)&HALTED);i++) delay(1000);
    if (rd(DSTS)&HALTED) { fail("controller run timeout");return; }
    g_msg_lastx=-1;
    thread_t thread;
    if (kernel_thread_start(usb_thread,NULL,&thread)!=KERN_SUCCESS) {
        fail("poll thread creation");wr(DCTL,rd(DCTL)&~RUN);return;
    }
    thread_deallocate(thread);
    u.stage="running";
    printf("MUYU USB: running; DMA=%llx; serial MUYUDBG001; awaiting enumeration\n",u.pa);

}
