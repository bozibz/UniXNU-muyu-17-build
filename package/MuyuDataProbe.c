#include <sys/param.h>
#include <sys/namei.h>          /* struct nameidata, needed before vnode_if.h is pulled in */
#include <sys/stat.h>           /* S_IFMT/VDIR/VREG */
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wvisibility"   /* vnode_if.h forward-declares struct nameidata */
#include <sys/vnode.h>
#pragma clang diagnostic pop
#include <sys/uio.h>
#include <sys/uio_internal.h>   /* struct uio itself (sys/uio.h only declares the helpers here) */
#include <sys/dirent.h>
#include <sys/proc.h>
#include <sys/proc_internal.h>
#include <sys/kauth.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <pexpert/pexpert.h>

/* IOKit's printf/sleep and a few private kernel functions are not in the exported headers this file sees. */
extern void IOLog(const char *format, ...) __attribute__((format(printf, 1, 2)));
extern void IOSleep(unsigned milliseconds);
extern int vn_rdwr_64(enum uio_rw rw, struct vnode *vp, uint64_t base, int64_t len, off_t offset,
    enum uio_seg segflg, int ioflg, kauth_cred_t cred, int64_t *aresid, void *p);
extern int vnode_lookup(const char *path, int flags, vnode_t *vpp, vfs_context_t ctx);
extern void thread_terminate(thread_t thread);

#define MUYU_DATA_PREFIX "MUYU DATA "
#define MUYU_DATA_READ 192

struct mu_data_target {
    const char *path;
    int read;                    // bytes to read back (0 = existence and size only)
};

// The paths that decide whether the lock screen can be passed without a passcode: MobileKeyBag's two
// bags, the migration/Setup preferences (seed vs live overlay), and the two "is this device provisioned"
// markers that AppSandbox/Setup read.
static const struct mu_data_target mu_data_targets[] = {
    {"/private/var/keybags", 0},
    {"/private/var/keybags/systembag.kb", 32},
    {"/private/var/keybags/userbag.kb", 32},
    {"/private/var/db/datamigrator", 0},
    {"/private/var/db/os_eligibility", 0},
    {"/private/var/mobile/Library/Preferences/com.apple.migration.plist", 96},
    {"/private/var/mobile/Library/Preferences/com.apple.purplebuddy.plist", 48},
    {"/private/var/mobile/Library/Preferences/com.apple.springboard.plist", 48},
    {"/private/var/mobile/Library/Preferences/.GlobalPreferences.plist", 48},
};

static void mu_data_hex(char *out, size_t cap, const uint8_t *data, int len) {
    size_t at = 0;
    for (int i = 0; i < len && at + 3 < cap; ++i) {
        static const char digits[] = "0123456789abcdef";
        out[at++] = digits[data[i] >> 4];
        out[at++] = digits[data[i] & 15];
    }
    out[at] = 0;
}

// Rename one file inside a directory. Used by mode 2 to move the keybags out of the way: the synthetic
// Data volume has no per-file keys, so a keybag that asks for a passcode nobody knows only blocks PreBoard.
static int mu_data_move(const char *dir, const char *leaf, const char *renamed) {
    vfs_context_t ctx = vfs_context_current();
    vnode_t dvp = NULL, vp = NULL;
    if (vnode_lookup(dir, 0, &dvp, ctx) || !dvp) return -1;
    char path[256];
    int at = 0;
    for (const char *c = dir; *c && at < (int)sizeof path - 1; ++c) path[at++] = *c;
    if (at && path[at - 1] != '/' && at < (int)sizeof path - 1) path[at++] = '/';
    for (const char *c = leaf; *c && at < (int)sizeof path - 1; ++c) path[at++] = *c;
    path[at] = 0;
    if (vnode_lookup(path, 0, &vp, ctx) || !vp) { vnode_put(dvp); return -2; }
    struct componentname from, to;
    memset(&from, 0, sizeof from);
    memset(&to, 0, sizeof to);
    from.cn_nameptr = (char *)leaf;    from.cn_namelen = strlen(leaf);    from.cn_flags = ISLASTCN;
    to.cn_nameptr   = (char *)renamed; to.cn_namelen   = strlen(renamed); to.cn_flags   = ISLASTCN;
    // BSD_KERNEL_PRIVATE builds keep a context pointer in the componentname; the kext headers here do not.
#ifdef BSD_KERNEL_PRIVATE
    from.cn_context = ctx;
    to.cn_context   = ctx;
#endif
    const int rc = VNOP_RENAME(dvp, vp, &from, dvp, NULL, &to, ctx);
    vnode_put(vp);
    vnode_put(dvp);
    return rc;
}

// One line per process: pid, state, name and the address it sleeps on. A hang shows up here as a
// process sitting in S (or U) on a wait channel, which userland logging (os_log) cannot show us.
static int mu_data_proc_line(proc_t p, void *arg) {
    int *shown = (int *)arg;
    if (*shown >= 24 || p->p_stat == SZOMB) return 0;
    // Match by name: the process order is not pid order, so a "first N" cut misses SpringBoard.
    static const char *const want[] = {
        "launchd", "backboardd", "SpringBoard", "PosterBoard", "WindowServer",
        "datamigrator", "keybagd", "lockdownd", "mobileactivationd", "tccd",
    };
    bool hit = false;
    for (unsigned i = 0; i < sizeof want / sizeof want[0]; ++i) {
        const char *n = want[i];
        const char *c = p->p_comm;
        while (*n && *c) {
            const char *a = n; const char *b = c;
            while (*a && *b && *a == *b) { ++a; ++b; }
            if (!*a) { hit = true; break; }
            ++c;
        }
        if (hit) break;
    }
    if (!hit) return 0;
    IOLog(MUYU_DATA_PREFIX "P %d stat=%d %s\n", (int)p->p_pid, (int)p->p_stat, p->p_comm);
    *shown += 1;
    return 0;
}

// The owned AppleKeyStore (package/KeyStoreProbe.cpp) counts what libaks asked and what it answered.
extern volatile unsigned native_aks_opens;
extern volatile unsigned native_aks_calls;
extern int native_aks_state_enabled(void);
extern unsigned native_aks_lock_state(void);
extern void native_aks_dump(void);

static void mu_data_aks(void) {
    IOLog(MUYU_DATA_PREFIX "AKS enabled=%d sls=%u opens=%u calls=%u\n",
        native_aks_state_enabled(), native_aks_lock_state(),
        (unsigned)native_aks_opens, (unsigned)native_aks_calls);
    // Per-selector rows are ~40 B each and the panic text buffer is 2401 B, so the summary above is all
    // that survives a dump; native_aks_dump() stays available for a console-only boot.
}

static void mu_data_procs(void) {
    int shown = 0;
    IOLog(MUYU_DATA_PREFIX "processes:\n");
    proc_iterate(PROC_ALLPROCLIST, mu_data_proc_line, &shown, NULL, NULL);
}

// The ten highest PIDs and their names: on this boot the beat shows every counter frozen well before the
// progress bar fills, so the question is where the boot stopped, and the newest processes are that place.
#define MU_DATA_NEWEST 10
static void mu_data_newest(void) {
    struct entry { int pid; char name[17]; } top[MU_DATA_NEWEST];
    memset(top, 0, sizeof top);
    proc_list_lock();
    for (proc_t p = allproc.lh_first; p != NULL; p = p->p_list.le_next) {
        const int pid = (int)p->p_pid;
        if (pid <= top[MU_DATA_NEWEST - 1].pid) continue;
        int at = MU_DATA_NEWEST - 1;
        while (at > 0 && top[at - 1].pid < pid) {
            top[at] = top[at - 1];
            at--;
        }
        top[at].pid = pid;
        for (unsigned i = 0; i < sizeof top[at].name - 1 && p->p_comm[i]; ++i) top[at].name[i] = p->p_comm[i];
        top[at].name[sizeof top[at].name - 1] = 0;
    }
    proc_list_unlock();
    for (unsigned i = 0; i < MU_DATA_NEWEST; ++i) {
        if (top[i].pid) IOLog(MUYU_DATA_PREFIX "new %d %s\n", top[i].pid, top[i].name);
    }
}

// Enumerate a directory: the keybag names are a guess, the listing is the fact. Prints at most 8 entries.
static void mu_data_list(const char *dir) {
    vfs_context_t ctx = vfs_context_current();
    vnode_t dvp = NULL;
    if (vnode_lookup(dir, 0, &dvp, ctx) || !dvp) {
        IOLog(MUYU_DATA_PREFIX "list %s: missing\n", dir);
        if (dvp) vnode_put(dvp);
        return;
    }
    char buf[1024];
    uio_t auio = uio_create(1, 0, UIO_SYSSPACE, UIO_READ);
    if (auio == NULL) {
        IOLog(MUYU_DATA_PREFIX "list %s: no uio\n", dir);
        vnode_put(dvp);
        return;
    }
    uio_addiov(auio, (user_addr_t)(uintptr_t)buf, sizeof buf);
    int eoff = 0, num = 0;
    const int rc = VNOP_READDIR(dvp, auio, 0, &eoff, &num, ctx);
    int used = 0;
    if (rc == 0) {
        used = (int)sizeof buf - (int)uio_resid(auio);
    }
    uio_free(auio);
    vnode_put(dvp);
    if (rc) {
        IOLog(MUYU_DATA_PREFIX "list %s rc=%d\n", dir, rc);
        return;
    }
    int at = 0, shown = 0;
    while (at + (int)sizeof(struct dirent) <= used && shown < 8) {
        const struct dirent *de = (const struct dirent *)(buf + at);
        if (de->d_reclen == 0 || at + de->d_reclen > used) break;
        if (de->d_namlen && de->d_name[0] != '.') {
            IOLog(MUYU_DATA_PREFIX "list %s/%s type=%u\n", dir, de->d_name, (unsigned)de->d_type);
            shown++;
        }
        at += de->d_reclen;
    }
    if (!shown) IOLog(MUYU_DATA_PREFIX "list %s: empty\n", dir);
}

static void __attribute__((unused)) mu_data_report(void) {
    vfs_context_t ctx = vfs_context_current();
    for (unsigned i = 0; i < sizeof mu_data_targets / sizeof mu_data_targets[0]; ++i) {
        const struct mu_data_target *target = &mu_data_targets[i];
        vnode_t vp = NULL;
        const int rc = vnode_lookup(target->path, 0, &vp, ctx);
        if (rc || !vp) {
            IOLog(MUYU_DATA_PREFIX "%s = missing (%d)\n", target->path, rc);
            if (vp) vnode_put(vp);
            continue;
        }
        struct vnode_attr va;
        VATTR_INIT(&va);
        VATTR_WANTED(&va, va_data_size);
        VATTR_WANTED(&va, va_mode);
        const int arc = vnode_getattr(vp, &va, ctx);
        const char kind = arc ? '?' : ((va.va_mode & S_IFMT) == VDIR ? 'd' :
                                      (va.va_mode & S_IFMT) == VREG ? 'f' : 'o');
        if (!target->read) {
            IOLog(MUYU_DATA_PREFIX "%s = %c size=%llu%s\n", target->path, kind,
                (unsigned long long)(arc ? 0 : va.va_data_size), arc ? " attr-err" : "");
            vnode_put(vp);
            continue;
        }
        uint8_t buf[MUYU_DATA_READ];
        int len = target->read;
        if (len > MUYU_DATA_READ) len = MUYU_DATA_READ;
        int64_t resid = len;
        const int rd = vn_rdwr_64(UIO_READ, vp, (uint64_t)(uintptr_t)buf, len, 0, UIO_SYSSPACE, 0,
            kauth_cred_get(), &resid, NULL);
        vnode_put(vp);
        if (rd) {
            IOLog(MUYU_DATA_PREFIX "%s = %c size=%llu read-err=%d\n", target->path, kind,
                (unsigned long long)(arc ? 0 : va.va_data_size), rd);
            continue;
        }
        len -= (int)resid;
        // Only the first 4 bytes: the size is the interesting part for a dump, and the panic text buffer
        // has to hold the process list as well.
        char hex[9];
        mu_data_hex(hex, sizeof hex, buf, len > 4 ? 4 : len);
        IOLog(MUYU_DATA_PREFIX "%s = %c size=%llu head=%s rd=%d\n", target->path, kind,
            (unsigned long long)(arc ? 0 : va.va_data_size), hex, len);
    }
}

static void mu_data_thread(void *arg, wait_result_t result) {
    (void)result;
    unsigned delay = 20;
    PE_parse_boot_argn("muyu-data-delay", &delay, sizeof delay);
    if (delay > 600) delay = 600;
    if (delay) IOSleep(delay * 1000);
    IOLog(MUYU_DATA_PREFIX "probe begin (mode %u)\n", (unsigned)(uintptr_t)arg);
    mu_data_list("/private/var/keybags");
    mu_data_procs();
    mu_data_aks();
    // Last, so it survives the 2401-byte truncation: the newest PIDs are the most useful rows in this dump.
    mu_data_newest();
    // mu_data_report() is not called any more: the heartbeat line already carries the same file sizes every
    // minute (sbp/mig/dm/pb), and those rows only pushed the newer data out of the panic text buffer.
    if ((unsigned)(uintptr_t)arg >= 2) {
        // Mode 2: PreBoard's passcode prompt comes from a keybag nobody can unlock on this synthetic Data
        // volume, so move the bags aside (reversible: the .off files stay next to them).
        static const char *const bags[] = { "systembag.kb", "userbag.kb", "backupbag.kb" };
        for (unsigned i = 0; i < sizeof bags / sizeof bags[0]; ++i) {
            char renamed[64];
            int at = 0;
            for (const char *c = bags[i]; *c && at < (int)sizeof renamed - 5; ++c) renamed[at++] = *c;
            renamed[at++] = '.'; renamed[at++] = 'o'; renamed[at++] = 'f'; renamed[at++] = 'f'; renamed[at] = 0;
            const int rc = mu_data_move("/private/var/keybags", bags[i], renamed);
            IOLog(MUYU_DATA_PREFIX "move %s -> %s rc=%d\n", bags[i], renamed, rc);
        }
    }
    IOLog(MUYU_DATA_PREFIX "probe end\n");
    (void)thread_terminate(current_thread());
}

// ---- heartbeat -----------------------------------------------------------------------------------
// One snapshot says what the machine looks like at one moment; a stalled boot needs the trend. Every
// interval this logs a single short line, so the 2401-byte panic text holds ~25 of them and a dump
// later in the boot shows whether anything was still moving and (with the swap counters) when it
// stopped. Fields: uptime seconds, process count, launchd/backboardd/SpringBoard process states,
// display swaps and the uptime of the last swap, AKS opens/calls, and two file sizes that the setup
// and migration flow touch.
static char mu_data_stat_letter(int stat) {
    switch (stat) {
    case SIDL: return 'I';
    case SRUN: return 'R';
    case SSLEEP: return 'S';
    case SSTOP: return 'T';
    case SZOMB: return 'Z';
    default: return '?';
    }
}

static void mu_data_hb_census(unsigned *count, char states[3]) {
    static const char *const want[3] = { "launchd", "backboardd", "SpringBoard" };
    unsigned total = 0;
    states[0] = states[1] = states[2] = '-';
    proc_list_lock();
    for (proc_t p = allproc.lh_first; p != NULL; p = p->p_list.le_next) {
        total++;
        for (unsigned i = 0; i < 3; ++i) {
            if (states[i] == '-' && !strcmp(p->p_comm, want[i])) states[i] = mu_data_stat_letter(p->p_stat);
        }
    }
    proc_list_unlock();
    *count = total;
}

// Size of a file, or -1 when it does not exist. Same lookup the full report uses.
static long long mu_data_size(const char *path) {
    vnode_t vp = NULL;
    if (vnode_lookup(path, 0, &vp, vfs_context_current()) || !vp) {
        if (vp) vnode_put(vp);
        return -1;
    }
    struct vnode_attr va;
    VATTR_INIT(&va);
    VATTR_WANTED(&va, va_data_size);
    const int rc = vnode_getattr(vp, &va, vfs_context_current());
    vnode_put(vp);
    return rc ? -1 : (long long)va.va_data_size;
}

extern void native_legacy_swap_counters(unsigned *swaps, unsigned *last_swap_seconds) __attribute__((weak_import));
extern unsigned native_legacy_uptime_seconds(void) __attribute__((weak_import));

static void mu_data_heartbeat(void *arg, wait_result_t result) {
    unsigned period = (unsigned)(uintptr_t)arg;
    (void)result;
    if (period < 10) period = 10;
    for (;;) {
        IOSleep(period * 1000);
        unsigned count = 0, swaps = 0, last = 0, uptime = 0;
        char states[3];
        mu_data_hb_census(&count, states);
        if (&native_legacy_swap_counters != NULL) native_legacy_swap_counters(&swaps, &last);
        if (&native_legacy_uptime_seconds != NULL) uptime = native_legacy_uptime_seconds();
        IOLog(MUYU_DATA_PREFIX "HB t=%u n=%u ld=%c bb=%c sb=%c sw=%u@%u aks=%u/%u sbp=%lld mig=%lld dm=%d pb=%d\n",
            uptime, count, states[0], states[1], states[2], swaps, last,
            (unsigned)native_aks_opens, (unsigned)native_aks_calls,
            mu_data_size("/private/var/mobile/Library/Preferences/com.apple.springboard.plist"),
            mu_data_size("/private/var/mobile/Library/Preferences/com.apple.migration.plist"),
            mu_data_size("/private/var/db/datamigrator") >= 0,
            mu_data_size("/private/var/mobile/Library/Preferences/com.apple.purplebuddy.plist") >= 0);
    }
}

static void mu_data_heartbeat_start(void) {
    unsigned period = 0;
    thread_t thread = NULL;
    if (!PE_parse_boot_argn("muyu-data-hb", &period, sizeof period) || period == 0) return;
    if (kernel_thread_start((thread_continue_t)mu_data_heartbeat, (void *)(uintptr_t)period, &thread) == KERN_SUCCESS)
        thread_deallocate(thread);
    else IOLog(MUYU_DATA_PREFIX "heartbeat thread failed\n");
}

extern void muyu_data_probe_start(void);
void muyu_data_probe_start(void) {
    unsigned mode = 0;
    // The heartbeat is independent of the one-shot probe: it is the only way to see a trend.
    mu_data_heartbeat_start();
    if (!PE_parse_boot_argn("muyu-data-probe", &mode, sizeof mode) || mode == 0) return;
    thread_t thread = NULL;
    if (kernel_thread_start((thread_continue_t)mu_data_thread, (void *)(uintptr_t)mode, &thread) == KERN_SUCCESS)
        thread_deallocate(thread);
    else IOLog(MUYU_DATA_PREFIX "probe thread failed\n");
}
