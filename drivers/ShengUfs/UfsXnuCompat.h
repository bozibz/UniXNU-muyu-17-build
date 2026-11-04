/* SPDX-License-Identifier: MIT */
/*
 * UfsXnuCompat.h - the kernel APIs the UFS driver used that XNU 12377 (the iPadOS 27 line) changed.
 * Both spellings stay in the tree so the driver keeps building on the 17.7.11 line.
 *
 *   * kmem_alloc() lost its (kma_flags_t, vm_tag_t) arguments in 12377; the flags only said "kernel
 *     object + data" and the tag VM_KERN_MEMORY_SECURITY, which the new signature does not take.
 *   * vm_map_protect() is gone; mach_vm_protect() is the same operation on both lines.
 *   * kmem_free() is kernel-private-only in 12377 (behind XNU_KERNEL_PRIVATE in vm_kern.h) while an
 *     external archive can still call mach_vm_deallocate(). It does not panic on bad arguments, which
 *     is acceptable here: the only caller frees the range it allocated itself.
 *
 * The three forward declarations below state exactly the prototypes used; on 12377 the headers that
 * carry them are not visible from a non-XNU_KERNEL_PRIVATE translation unit.
 */
#ifndef UFS_XNU_COMPAT_H
#define UFS_XNU_COMPAT_H

#include <mach/vm_types.h>
#include <mach/vm_prot.h>

#if defined(XNU_MUYU_XNU_VERSION) && XNU_MUYU_XNU_VERSION >= 12377
#define UFS_XNU_12377 1
#endif

#if defined(UFS_XNU_12377)
extern "C" kern_return_t kmem_alloc(vm_map_t map, vm_offset_t *address, vm_size_t bytes);
extern "C" kern_return_t mach_vm_protect(vm_map_t map, mach_vm_address_t address,
    mach_vm_size_t bytes, boolean_t set_maximum, vm_prot_t protection);
extern "C" kern_return_t mach_vm_deallocate(vm_map_t map, mach_vm_address_t address,
    mach_vm_size_t bytes);
#else
#include <vm/vm_kern.h>
#include <vm/vm_map.h>
#endif

/* Wired kernel memory for the verified-root policy table, tagged as security data on the 17 line. */
static inline kern_return_t
ufs_alloc_security_storage(vm_offset_t *address, vm_size_t bytes)
{
#if defined(UFS_XNU_12377)
	return kmem_alloc(kernel_map, address, bytes);
#else
	return kmem_alloc(kernel_map, address, bytes,
	    static_cast<kma_flags_t>(KMA_KOBJECT | KMA_DATA), VM_KERN_MEMORY_SECURITY);
#endif
}

/* Make the policy table read-only once it is written. */
static inline kern_return_t
ufs_protect_read_only(vm_offset_t address, vm_size_t bytes)
{
#if defined(UFS_XNU_12377)
	return mach_vm_protect(kernel_map, address, bytes, TRUE, VM_PROT_READ);
#else
	return vm_map_protect(kernel_map, address, address + bytes, VM_PROT_READ, TRUE);
#endif
}

static inline void
ufs_free_security_storage(vm_offset_t address, vm_size_t bytes)
{
#if defined(UFS_XNU_12377)
	(void)mach_vm_deallocate(kernel_map, address, bytes);
#else
	kmem_free(kernel_map, address, bytes);
#endif
}

#endif /* UFS_XNU_COMPAT_H */
