// SPDX-License-Identifier: BSD-2-Clause-Patent
// The authenticated policy view the source storage runtime publishes (muyu, iPadOS 27 line).
//
// uke keeps this struct inside drivers/NativeUfs/NativeUfs.cpp; the loader in SourceRootPolicies.cpp
// needs it too, so it lives here and the UFS provider includes this header when it is ported to the
// public IOStorageFamily interface. A policy is either the whole envelope or only its signed header
// (then the hash table is a userdata object the storage runtime reads and checks itself).
#ifndef MUYU_VERIFIED_BLOCK_POLICY_H
#define MUYU_VERIFIED_BLOCK_POLICY_H

#include <stdint.h>

struct NativeVerifiedBlockPolicy {
	const uint8_t *bytes;
	uint64_t size;
};

#endif /* MUYU_VERIFIED_BLOCK_POLICY_H */
