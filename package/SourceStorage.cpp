// SPDX-License-Identifier: BSD-2-Clause-Patent
// Start the source-built UFS adapter after the original storage family loads (muyu, iPadOS 27 line).
//
// Modelled on uke's package/SourceStorage.cpp (UniXNU-uke c7aefb9): the launcher is deliberately small and
// does not touch UFS registers. It loads the original IOStorageFamily, proves the public
// IOBlockStorageDevice layout still matches the source one, registers the source-built classes through
// their initialisers, and then creates and starts the provider through the public interface.
//
// What muyu still has to supply (the compiler names are declared below, so the archive links and a
// missing piece fails with a sentence instead of an undefined symbol):
//   * native_ufs_source_create()  - a UFS provider built on the public IOBlockStorageDevice interface,
//     i.e. the port of drivers/ShengUfs/NativeUfs.cpp away from the 17 line's original-ABI binding.
//   * native_ufs_root_step / native_ufs_root_failure - our driver's progress record for diagnostics.
//   * native_source_root_policies_load() - the authenticated root policy handoff (owner-root TRUST).
// Until those exist they are weak stubs that report themselves; nothing here pretends they work.
#include <IOKit/IOService.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <IOKit/storage/IOBlockStorageDevice.h>
#include <libkern/OSKextLib.h>
#include <mach/kmod.h>
#include <pexpert/pexpert.h>
#include <IOKit/IOLib.h>

extern kmod_info_t g_kernel_kmod_info;
// The public IOStorageFamily sources and the board's own sources each provide one of these.
extern "C" void source_storage_base_init(void) __asm__("__GLOBAL__sub_I_IOBlockStorageDevice.cpp");
extern "C" void source_storage_ufs_init(void) __asm__("__GLOBAL__sub_I_NativeUfs.cpp");
extern "C" void source_storage_ram_init(void) __asm__("__GLOBAL__sub_I_NativeRamBlock.cpp");
extern "C" IOService *native_ufs_source_create(void);
extern "C" bool native_source_root_policies_load(void);
extern "C" volatile uint32_t native_ufs_root_step;
extern "C" const char *volatile native_ufs_root_failure;

/*
 * No weak stand-ins here: the archive that carries this launcher always carries the three
 * implementations too (drivers/ShengUfs/NativeUfs.cpp for the provider and its root-step state,
 * package/SourceRootPolicies.cpp for the handoff). A weak definition in this object would win over
 * the real one whenever the linker picked this member first, which it does for the launcher symbol -
 * the kernel would then boot with a stub provider and no diagnostics.
 */

// NativeUfs polls the optional board progress diagnostic while it reads root windows. The minimal
// source kernel does not carry it, so a weak definition keeps the archive linkable; the real one wins
// when that component is built in (the 17 line builds it in package/BootProgress.c).
extern "C" __attribute__((weak)) void boot_progress_poll(void) {}

static const OSMetaClass *storageParent;
static IOService *storageDriver;
static bool attempted;

/* Used only by the deferred source IOBlockStorageDevice metaclass constructor. */
extern "C" const OSMetaClass *
native_source_storage_parent(void)
{
	if (!storageParent) {
		panic("SourceStorage: metaclass initialized before the original family");
	}
	return storageParent;
}

extern "C" void
native_source_storage_start(void)
{
	unsigned enabled = 0;
	/* Boot argument names the 27 line explicitly so a 17-line boot cannot reach this code. */
	if (!PE_parse_boot_argn("muyu-source-storage", &enabled, sizeof(enabled)) || enabled != 1 || attempted) {
		return;
	}
	attempted = true;

	OSReturn status = OSKextLoadKextWithIdentifier("com.apple.iokit.IOStorageFamily");
	if (status != kOSReturnSuccess) {
		panic("SourceStorage: IOStorageFamily load failed %x", status);
	}
	const OSSymbol *name = OSSymbol::withCString("IOBlockStorageDevice");
	storageParent = name ? OSMetaClass::getMetaClassWithName(name) : nullptr;
	if (name) {
		name->release();
	}
	/* The public source definition and the running family must agree on the base layout. */
	if (!storageParent || storageParent->getClassSize() != sizeof(IOBlockStorageDevice)) {
		panic("SourceStorage: original base layout %u differs from source %lu",
		    storageParent ? storageParent->getClassSize() : 0, sizeof(IOBlockStorageDevice));
	}
	if (!native_source_root_policies_load()) {
		panic("SourceStorage: root policy handoff or authentication failed");
	}
	status = OSKextLoadKextWithIdentifier("com.apple.filesystems.apfs");
	if (status != kOSReturnSuccess) {
		panic("SourceStorage: APFS load failed %x", status);
	}
	void *handle = OSMetaClass::preModLoad(g_kernel_kmod_info.name);
	if (!handle) {
		panic("SourceStorage: cannot begin class registration");
	}
	source_storage_base_init();
	source_storage_ufs_init();
	source_storage_ram_init();
	status = OSMetaClass::postModLoad(handle);
	if (status != kOSReturnSuccess) {
		panic("SourceStorage: class registration failed %x", status);
	}

	OSDictionary *matching = IOService::serviceMatching("IOPlatformExpertDevice");
	IOService *provider = matching ? IOService::waitForMatchingService(matching, 5000000000ULL) : nullptr;
	if (!provider) {
		panic("SourceStorage: platform root unavailable");
	}
	storageDriver = native_ufs_source_create();
	if (!storageDriver) {
		panic("SourceStorage: cannot allocate the UFS service");
	}
	const char *step = !storageDriver->init() ? "init" : !storageDriver->attach(provider) ? "attach" :
	    !storageDriver->start(provider) ? "start" : nullptr;
	if (step) {
		panic("SourceStorage: UFS %s failed (root step %u: %s)", step, native_ufs_root_step,
		    native_ufs_root_failure ? native_ufs_root_failure : "no reason recorded");
	}
	printf("SourceStorage: UFS started through the public IOStorageFamily interface\n");
}
