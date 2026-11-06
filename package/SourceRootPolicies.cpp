// SPDX-License-Identifier: BSD-2-Clause-Patent
// Authenticate the firmware's root-policy sidecar with a compiled-in owner key (muyu, iPadOS 27 line).
//
// Model: uke's package/SourceRootPolicies.cpp. The loader takes the /chosen/memory-map "RootPolicies"
// sidecar the loader filled in (physical address + length), copies it into wired kernel memory, protects
// it read-only, then authenticates every envelope in the VROOTB01 bundle against the compiled-in owner key.
// Nothing here trusts a firmware-supplied hash: each envelope's *signed header* is verified, and its digest
// must equal the value generated for this OS line (package/OwnerBootIdentity.h).
//
// Unlike the 17.7.11 line, this file only targets XNU 12377. Two ABI facts shaped the copy below:
//   - kmem_alloc inside 12377 takes five arguments (vm_map, vm_offset_t *, vm_size_t, int flags, vm_tag_t),
//     while vm_kern.h hands a kext a three-argument declaration. Calling it three-argument leaves flags/tag
//     undefined, and a zero tag trips vm_resident.c:3408 ("wired page with VM_KERN_MEMORY_NONE").
//   - IOMallocData() is the tagged IOKit allocator (a kalloc heap with its own VM tag), so the copy lands in
//     ordinary wired kernel data with no vm_object and nothing to protect afterwards.
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
// pexpert physical word read (osfmk/arm64/loose_ends.c): four bytes, no mapping.
extern "C" unsigned int ml_phys_read(vm_offset_t paddr);
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
	printf("SourceStorage: 2a read physical\n");
	/* Read the handoff with the pexpert physical word reader into tagged IOKit data. No
	 * IOMemoryDescriptor, no mapping, no vm_object, no untagged kmem_alloc:
	 *   - IOMemoryDescriptor::withPhysicalAddress(...)->map() inserted untagged pages and failed;
	 *   - the probe before ml_io_map() showed the registry lookup, the RootPolicies property
	 *     (physical 0x8c5c0000, length 1152) and withPhysicalAddress() were fine, so the fault lay
	 *     at or after the map, and ml_io_map() was still not enough;
	 *   - kmem_alloc() has a five-argument ABI in this kernel, so the three-argument call left the
	 *     page tag undefined and vm_page_insert() asserted on VM_KERN_MEMORY_NONE.
	 * 1152 bytes read four at a time needs no mapping at all. */
	void *allocationStorage = IOMallocData(static_cast<vm_size_t>(length));
	if (!allocationStorage) {
		return false;
	}
	const vm_offset_t allocation = reinterpret_cast<vm_offset_t>(allocationStorage);
	for (uint64_t at = 0; at < length; at += 4) {
		*reinterpret_cast<uint32_t *>(allocation + at) = ml_phys_read(physical + at);
	}
	const auto *bytes = reinterpret_cast<const uint8_t *>(allocation);
	const uint64_t magic = read64(bytes);
	if (magic != 0x313042544f4f5256ULL) { /* "VROOTB01" little-endian */
		panic("SourceStorage: at 2c bad magic %x %x", (unsigned)(magic & 0xffffffff), (unsigned)(magic >> 32));
	}
	printf("SourceStorage: 2c copied\n");
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
