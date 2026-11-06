// SPDX-License-Identifier: BSD-2-Clause-Patent
// Authenticate the firmware's root-policy sidecar with a compiled-in owner key (muyu, iPadOS 27 line).
//
// Model: uke's package/SourceRootPolicies.cpp. The loader takes the /chosen/memory-map "RootPolicies"
// sidecar the loader filled in (physical address + length), copies it into wired kernel memory, protects
// it read-only, then authenticates every envelope in the VROOTB01 bundle against the compiled-in owner key.
// Nothing here trusts a firmware-supplied hash: each envelope's *signed header* is verified, and its digest
// must equal the value generated for this OS line (package/OwnerBootIdentity.h).
//
// Unlike the 17.7.11 line, this file only targets XNU 12377: kmem_alloc has no flags/tag arguments there and
// the protection call is mach_vm_protect.
#include <IOKit/IOService.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOLib.h>
#include <libkern/c++/OSData.h>
#include <mach/mach_vm.h>
// Arm64 declares these in osfmk/arm64 headers the kext build does not include; the
// signatures match osfmk/i386/machine_routines.h and the exported symbols.
extern "C" vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern "C" void ml_io_unmap(vm_offset_t addr, vm_size_t size);
#include <mach/vm_prot.h>
#include <vm/vm_kern.h>
#include <vm/vm_map.h>

#include "OwnerBootIdentity.h"
#include "VerifiedBlockPolicy.h"
#include "verified_root.h"

extern "C" {
/* The owner key and the crypto provider are compiled in by the VerifiedRoot kit. */
extern const uint8_t verified_root_key[];
extern const struct vr_crypto vr_xnu_crypto;
NativeVerifiedBlockPolicy verified_root_policies[16];
uint64_t verified_root_policy_count;
extern "C" kern_return_t kmem_alloc(vm_map_t, vm_offset_t *, vm_size_t);
extern "C" kern_return_t mach_vm_protect(vm_map_t, mach_vm_address_t, mach_vm_size_t, boolean_t, vm_prot_t);
}

static uint64_t
read64(const uint8_t *bytes)
{
	uint64_t value = 0;
	for (unsigned index = 0; index != 8; ++index) {
		value |= uint64_t(bytes[index]) << (index * 8);
	}
	return value;
}

extern "C" bool
native_source_root_policies_load(void)
{
	if (verified_root_policy_count) {
		return true;
	}
	IORegistryEntry *entry = IORegistryEntry::fromPath("/chosen/memory-map", gIODTPlane);
	if (!entry) {
		return false;
	}
	OSData *property = OSDynamicCast(OSData, entry->getProperty("RootPolicies"));
	uint64_t physical = 0, length = 0;
	if (property && property->getLength() == 16) {
		const auto *data = static_cast<const uint8_t *>(property->getBytesNoCopy());
		physical = read64(data);
		length = read64(data + 8);
	}
	entry->release();
	if (!physical || (physical & 4095) || length < 32 || length > 128ULL * 1024 * 1024 ||
	    physical > UINT64_MAX - length) {
		return false;
	}
	printf("SourceStorage: 2a descriptor\n");
	IOMemoryDescriptor *descriptor = IOMemoryDescriptor::withPhysicalAddress(physical, length, kIODirectionIn);
	if (!descriptor) {
		return false;
	}
	/* Read the handoff through the pexpert physical window instead of mapping the
	 * descriptor. A raw physical IOMemoryDescriptor map inserts its pages with no VM
	 * tag, and this kernel's vm_page_insert_internal() asserts on "wired page with
	 * VM_KERN_MEMORY_NONE" (2026-11-06: panic right after this point, pc =
	 * _vm_page_insert_internal.cold.1+0xc). ml_io_map builds a device window and
	 * needs no vm_object, so nothing is inserted. */
	printf("SourceStorage: 2b map\n");
	vm_offset_t window = ml_io_map(physical, length);
	if (!window) {
		descriptor->release();
		return false;
	}
	vm_offset_t allocation = 0;
	const vm_size_t allocationBytes = round_page(static_cast<vm_size_t>(length));
	const kern_return_t allocated = kmem_alloc(kernel_map, &allocation, allocationBytes);
	if (allocated == KERN_SUCCESS) {
		memcpy(reinterpret_cast<void *>(allocation),
		    reinterpret_cast<const void *>(window), length);
	}
	ml_io_unmap(window, length);
	descriptor->release();
	printf("SourceStorage: 2c kmem_alloc %llx\n", (unsigned long long)allocation);
	if (allocated != KERN_SUCCESS) {
		return false;
	}
	/* Protect the copy before authenticating it: no later firmware or source mapping can change it. */
	printf("SourceStorage: 2d protect\n");
	if (mach_vm_protect(kernel_map, allocation, allocationBytes, TRUE, VM_PROT_READ) != KERN_SUCCESS) {
		return false;
	}
	const auto *bytes = reinterpret_cast<const uint8_t *>(allocation);
	if (memcmp(bytes, "VROOTB01", 8) || read64(bytes + 8) != 1 || read64(bytes + 24) != length) {
		return false;
	}
	const uint64_t count = read64(bytes + 16);
	if (!count || count > 16 || 32 + count * 16 > length) {
		return false;
	}
	uint64_t cursor = 32 + count * 16;
	for (uint64_t index = 0; index != count; ++index) {
		const uint64_t offset = read64(bytes + 32 + index * 16);
		const uint64_t size = read64(bytes + 40 + index * 16);
		if (offset != cursor || size < VR_HEADER_BYTES + VR_SIGNATURE_BYTES || size > length - cursor) {
			return false;
		}
		/* Either the whole envelope or only its signed header (the hash table is a userdata object). */
		struct vr_view view = {};
		const bool headerOnly = size == VR_HEADER_BYTES + VR_SIGNATURE_BYTES;
		const int opened = headerOnly ?
		    vr_open_header(bytes + offset, size, verified_root_key, 1, &vr_xnu_crypto, &view) :
		    vr_open(bytes + offset, size, verified_root_key, 1, &vr_xnu_crypto, &view);
		if (opened != VR_OK) {
			return false;
		}
		/* Bind the owner-domain identity to the exact signed headers this OS line expects. */
		uint8_t headerDigest[32];
		if (count != 2 || index >= 2 ||
		    !vr_xnu_crypto.sha256(bytes + offset, VR_HEADER_BYTES + VR_SIGNATURE_BYTES, headerDigest) ||
		    memcmp(headerDigest, owner_policy_header_sha256[index], sizeof(headerDigest))) {
			return false;
		}
		verified_root_policies[index] = {bytes + offset, size};
		cursor += size;
	}
	if (cursor != length) {
		return false;
	}
	IORegistryEntry *chosen = IORegistryEntry::fromPath("/chosen", gIODTPlane);
	OSData *identity = chosen ? OSDynamicCast(OSData, chosen->getProperty("boot-manifest-hash")) : nullptr;
	OSData *domain = chosen ? OSDynamicCast(OSData, chosen->getProperty("owner-boot-manifest-domain")) : nullptr;
	const bool bound = identity && identity->getLength() == sizeof(owner_boot_set_hash) &&
	    !memcmp(identity->getBytesNoCopy(), owner_boot_set_hash, sizeof(owner_boot_set_hash)) &&
	    domain && domain->getLength() == sizeof(owner_boot_set_domain) &&
	    !memcmp(domain->getBytesNoCopy(), owner_boot_set_domain, sizeof(owner_boot_set_domain));
	if (chosen) {
		chosen->release();
	}
	if (!bound) {
		return false;
	}
	verified_root_policy_count = count;
	return true;
}
