/* SPDX-License-Identifier: MIT */
/*
 * MuyuGpuRoleCompat.h - the process GPU role the native GPU driver asks for, on an OS that lacks it.
 *
 * The pinned NativeAdreno (uke's accepted iPadOS 27 baseline, commit 22abfe4) asks in
 * NativeAdrenoUserClient.cpp:
 *
 *     const int32_t role = task_get_gpu_role(current_task());
 *     const bool background = role == PRIO_DARWIN_GPU_BACKGROUND || role == PRIO_DARWIN_GPU_UTILITY;
 *
 * iPadOS 21H461 (17.7.11) has neither `task_get_gpu_role()` nor those two constants. It carries the
 * same information elsewhere: the task policy attribute TASK_POLICY_ROLE, which
 * proc_task_role_to_darwin_role() maps to PRIO_DARWIN_ROLE_* (bsd/kern/kern_resource.c,
 * proc_get_darwin_role). The role is therefore *read*, not invented; only the two newer constant names
 * are ours.
 *
 * Why this is a declaration only: the kernel headers that implement the read (kern/task.h,
 * kern/policy_internal.h) cannot be included from a C++ translation unit (ledger.h:151 uses `template`
 * as a parameter name). So the implementation is C, in MuyuGpuRoleCompat.c, compiled into the same
 * archive by scripts/build-adreno740-kits.py; C++ users see this header.
 *
 * Honest limits:
 *   * PRIO_DARWIN_GPU_BACKGROUND / _UTILITY are our mapping of the newer constants onto this OS's role
 *     enum (NON_UI / UI_NON_FOCAL). It is a hypothesis; an iOS 27 header would settle it. The only
 *     consumer picks which of the two Metal queue priorities the driver hands to the ring, so a wrong
 *     mapping changes priority selection, not correctness.
 *   * On 17.7.11 nothing sets a GPU role for an ordinary foreground app, so most tasks read
 *     TASK_UNSPECIFIED -> PRIO_DARWIN_ROLE_DEFAULT, which the driver treats as foreground.
 */
#ifndef MUYU_GPU_ROLE_COMPAT_H
#define MUYU_GPU_ROLE_COMPAT_H

#include <mach/mach_types.h>
#include <stdint.h>
#include <sys/resource.h>

/* The newer kernel's process GPU roles, mapped onto this OS's role enum. */
#ifndef PRIO_DARWIN_GPU_BACKGROUND
#define PRIO_DARWIN_GPU_BACKGROUND      PRIO_DARWIN_ROLE_NON_UI
#endif
#ifndef PRIO_DARWIN_GPU_UTILITY
#define PRIO_DARWIN_GPU_UTILITY         PRIO_DARWIN_ROLE_UI_NON_FOCAL
#endif

#ifdef __cplusplus
extern "C" {
#endif
/* The task's role in the PRIO_DARWIN_ROLE_* space the driver compares against. */
int8_t task_get_gpu_role(task_t task);
#ifdef __cplusplus
}
#endif

#endif /* MUYU_GPU_ROLE_COMPAT_H */
