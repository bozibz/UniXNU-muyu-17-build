// SPDX-License-Identifier: MIT
// Bring-up only: render the newest crash report from the Data volume through the panic screen.
//
// Userland logging on this image goes to logd, so a crashing daemon leaves nothing in the kernel
// message buffer; ReportCrash writes a report file on the Data volume instead. This file is
// compiled with the BSD-side flags (kern_exec.o record) so it can use the kernel's VFS.
//
// Boot arguments:
//   muyu-crash-dir=<path>    directory to read (default /private/var/mobile/Library/Logs/CrashReporter)
//   muyu-crash-bytes=<n>     bytes to render (default 1100)
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/vnode.h>
#include <sys/vnode_internal.h>
#include <sys/uio.h>
#include <sys/dirent.h>
#include <sys/fcntl.h>
#include <sys/errno.h>
#include <sys/proc_internal.h>
#include <pexpert/pexpert.h>
#include <kern/debug.h>

/* Bring-up: one line about userland at dump time. The display counters show the compositor made its
 * last swap at t; this says whether the processes that drive it still exist, so a frozen screen can be
 * told apart from a dead userland. */
static char
muyu_stat_letter(int stat)
{
	switch (stat) {
	case SIDL:
		return 'I';
	case SRUN:
		return 'R';
	case SSLEEP:
		return 'S';
	case SSTOP:
		return 'T';
	case SZOMB:
		return 'Z';
	default:
		return '?';
	}
}

void
muyu_proc_census(char *out, size_t cap)
{
	static const char *wanted[] = { "launchd", "backboardd", "SpringBoard", "LiveActivities" };
	int pids[4] = { 0, 0, 0, 0 };
	char states[4] = { '-', '-', '-', '-' };
	unsigned int total = 0, zombies = 0;
	proc_t p;
	int i;

	if (out == NULL || cap < 64) {
		return;
	}
	proc_list_lock();
	for (p = allproc.lh_first; p != NULL; p = p->p_list.le_next) {
		total++;
		if (p->p_stat == SZOMB) {
			zombies++;
		}
		for (i = 0; i < 4; i++) {
			if (pids[i] == 0 && strcmp(p->p_comm, wanted[i]) == 0) {
				pids[i] = (int)p->p_pid;
				states[i] = muyu_stat_letter(p->p_stat);
			}
		}
	}
	proc_list_unlock();
	snprintf(out, cap, "PROC n=%u zomb=%u launchd=%d%c backboardd=%d%c SpringBoard=%d%c liveact=%d%c\n",
	    total, zombies, pids[0], states[0], pids[1], states[1], pids[2], states[2],
	    pids[3], states[3]);
}

#define MUYU_CRASH_TEXT_BYTES 1201
#define MUYU_CRASH_READ_BYTES 32768

static char muyu_crash_text[MUYU_CRASH_TEXT_BYTES];
static char muyu_crash_path[320];
static char muyu_crash_body[MUYU_CRASH_READ_BYTES];

static int
muyu_read_head(const char *path, char *out, size_t cap, size_t max_read)
{
	vfs_context_t ctx = vfs_context_kernel();
	vnode_t vp = NULLVP;
	size_t got = 0;
	int error;

	if (cap < 2) {
		return EINVAL;
	}
	out[0] = '\0';
	error = vnode_open(path, O_RDONLY, 0, 0, &vp, ctx);
	if (error) {
		return error;
	}
	while (got + 1 < cap && got < max_read) {
		size_t chunk = cap - 1 - got;
		uio_t auio;
		size_t done;

		if (chunk > 512) {
			chunk = 512;
		}
		auio = uio_create(1, (off_t)got, UIO_SYSSPACE, UIO_READ);
		if (auio == NULL) {
			error = ENOMEM;
			break;
		}
		uio_addiov(auio, (uintptr_t)(out + got), chunk);
		error = VNOP_READ(vp, auio, 0, ctx);
		done = error ? 0 : chunk - (size_t)uio_resid(auio);
		uio_free(auio);
		if (error || done == 0) {
			break;
		}
		got += done;
	}
	vnode_close(vp, 0, ctx);
	out[got] = '\0';
	for (size_t i = 0; i < got; i++) {
		if (out[i] == '\0') {
			out[i] = '\n';
		}
	}
	return error;
}

static int
muyu_newest_name(const char *dir, char *name, size_t cap)
{
	vfs_context_t ctx = vfs_context_kernel();
	vnode_t dvp = NULLVP;
	char buf[1024];
	off_t offset = 0;
	int error, eofflag = 0, numdirent = 0, found = 0;

	error = vnode_open(dir, O_RDONLY, 0, 0, &dvp, ctx);
	if (error) {
		return error;
	}
	for (;;) {
		uio_t auio = uio_create(1, offset, UIO_SYSSPACE, UIO_READ);
		struct dirent *dp;
		size_t used;

		if (auio == NULL) {
			error = ENOMEM;
			break;
		}
		uio_addiov(auio, (uintptr_t)buf, sizeof(buf));
		error = VNOP_READDIR(dvp, auio, 0, &eofflag, &numdirent, ctx);
		used = sizeof(buf) - (size_t)uio_resid(auio);
		uio_free(auio);
		if (error) {
			break;
		}
		offset += (off_t)used;
		for (dp = (struct dirent *)buf; (char *)dp < buf + used;) {
			if (dp->d_reclen == 0) {
				break;
			}
			if (dp->d_namlen > 0 && !(dp->d_namlen == 1 && dp->d_name[0] == '.') &&
			    !(dp->d_namlen == 2 && dp->d_name[0] == '.' && dp->d_name[1] == '.')) {
				/* ReportCrash names its reports with the time, so the largest name is newest. */
				if (!found || strcmp(dp->d_name, name) > 0) {
					strlcpy(name, dp->d_name, cap);
					found = 1;
				}
			}
			dp = (struct dirent *)((char *)dp + dp->d_reclen);
		}
		if (eofflag || used == 0) {
			break;
		}
	}
	vnode_close(dvp, 0, ctx);
	return found ? 0 : ENOENT;
}

/* 2026-11-03 muyu bring-up: boots now reach launchd's boot tasks, and those tools fail with messages
 * like "FAILED TO FIND DISK NODE" because they look for the BSD device nodes of an APFS volume. The
 * kernel mounts root and the Data seed itself, so whether /dev/disk* exists at all decides whether the
 * fix is "publish volume IOMedia/BSD clients" or something else. Print the directory when the boot arg
 * muyu-dev-list=1 is present; it lands in the panic dump like any other log line. */
void
muyu_list_dev(void)
{
	vfs_context_t ctx = vfs_context_kernel();
	vnode_t dvp = NULLVP;
	char buf[1024];
	off_t offset = 0;
	int error, eofflag = 0, numdirent = 0;
	unsigned shown = 0;

	error = vnode_open("/dev", O_RDONLY, 0, 0, &dvp, ctx);
	if (error) {
		printf("DEV /dev: open failed %d\n", error);
		return;
	}
	for (;;) {
		uio_t auio = uio_create(1, offset, UIO_SYSSPACE, UIO_READ);
		struct dirent *dp;
		size_t used;

		if (auio == NULL) {
			break;
		}
		uio_addiov(auio, (uintptr_t)buf, sizeof(buf));
		error = VNOP_READDIR(dvp, auio, 0, &eofflag, &numdirent, ctx);
		used = sizeof(buf) - (size_t)uio_resid(auio);
		uio_free(auio);
		if (error) {
			printf("DEV /dev: readdir failed %d\n", error);
			break;
		}
		offset += (off_t)used;
		for (dp = (struct dirent *)buf; (char *)dp < buf + used;) {
			if (dp->d_reclen == 0) {
				break;
			}
			if (dp->d_namlen > 4 && strncmp(dp->d_name, "disk", 4) == 0 && shown < 24) {
				printf("DEV /dev/%s\n", dp->d_name);
				shown++;
			}
			dp = (struct dirent *)((char *)dp + dp->d_reclen);
		}
		if (eofflag || used == 0) {
			break;
		}
	}
	vnode_close(dvp, 0, ctx);
	printf("DEV /dev: %u disk nodes\n", shown);
}

extern void muyu_console_wake(void);

void
muyu_panic_crash_report(void)
{
	char dir[160];
	char name[128];
	char find[64];
	unsigned int cap = 1100;
	unsigned int offset = 0;
	size_t size;
	int error;

	if (!PE_parse_boot_argn("muyu-crash-dir", dir, (int)sizeof(dir)) || dir[0] == '\0') {
		strlcpy(dir, "/private/var/mobile/Library/Logs/CrashReporter", sizeof(dir));
	}
	if (PE_parse_boot_argn("muyu-crash-bytes", &cap, (int)sizeof(cap))) {
		if (cap < 100 || cap >= sizeof(muyu_crash_text)) {
			cap = 1100;
		}
	} else {
		cap = 1100;
	}
	muyu_console_wake();
	error = muyu_newest_name(dir, name, sizeof(name));
	if (error) {
		panic("CRASHLOG: list %s failed %d", dir, error);
	}
	snprintf(muyu_crash_path, sizeof(muyu_crash_path), "%s/%s", dir, name);
	error = muyu_read_head(muyu_crash_path, muyu_crash_body, sizeof(muyu_crash_body), sizeof(muyu_crash_body) - 1);
	if (error) {
		panic("CRASHLOG: read %s failed %d", muyu_crash_path, error);
	}
	size = strlen(muyu_crash_body);
	/* A crash report is JSON with the interesting part (termination / exception / asi) far past
	 * the header, so the dump can start at a marker or at an explicit offset instead of byte 0. */
	if (PE_parse_boot_argn("muyu-crash-find", find, (int)sizeof(find)) && find[0] != '\0') {
		char *hit = strnstr(muyu_crash_body, find, size);
		if (hit != NULL) {
			offset = (unsigned int)(hit - muyu_crash_body);
		}
	} else if (!PE_parse_boot_argn("muyu-crash-offset", &offset, (int)sizeof(offset))) {
		offset = 0;
	}
	if (offset >= size) {
		offset = 0;
	}
	if (size - offset < cap) {
		cap = (unsigned int)(size - offset);
	}
	memcpy(muyu_crash_text, muyu_crash_body + offset, cap);
	muyu_crash_text[cap] = '\0';
	panic("CRASHLOG %s @%u/%u:\n%s", muyu_crash_path, offset, (unsigned)size, muyu_crash_text);
}
