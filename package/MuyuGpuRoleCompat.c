/* SPDX-License-Identifier: MIT */
/* C side of MuyuGpuRoleCompat.h: read the task policy role the way proc_get_darwin_role() does. */
#include "MuyuGpuRoleCompat.h"
#include <kern/task.h>
#include <kern/policy_internal.h>

int8_t
task_get_gpu_role(task_t task)
{
	int role = proc_get_task_policy(task, TASK_POLICY_ATTRIBUTE, TASK_POLICY_ROLE);
	return (int8_t)proc_task_role_to_darwin_role(role);
}
