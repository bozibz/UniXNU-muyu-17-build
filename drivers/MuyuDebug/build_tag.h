/* SPDX-License-Identifier: MIT
 *
 * The build tag: one string, one place, printed everywhere.
 *
 * Nothing in this component may write a round number into a printf again. On 2026-11-08 a
 * reconstruction of the u151-u159 rounds concluded "the kernel source did not change" from the
 * version strings, because those strings were hardcoded per printf and one image carried both
 * "(u151)" and "(u155)". The conclusion was wrong.
 *
 * Where the value comes from, in order:
 *   1. build_tag_generated.h next to this file, written by scripts/build-tag.py (the normal path:
 *      no build-system change, the kernel picks the include up from the source directory);
 *   2. -DMUYU_BUILD_TAG='"27-scr-1108a"' for a build that can pass flags;
 *   3. "untagged" - visible on the panel, and scripts/build-tag.py check fails on it.
 *
 * Naming: <line>-<purpose>-<MMDD><letter>, see docs/BUILD-NAMING.md. The u-counter is retired.
 */
#ifndef MUYU_BUILD_TAG_HEADER
#define MUYU_BUILD_TAG_HEADER

#if defined(__has_include)
#  if __has_include("build_tag_generated.h")
#    include "build_tag_generated.h"
#  endif
#endif

#ifndef MUYU_BUILD_TAG
#define MUYU_BUILD_TAG "untagged"
#endif

/* "(27-scr-1108a)" - the parenthesised form the panel rows have always used. */
#define MUYU_TAG "(" MUYU_BUILD_TAG ")"

#endif /* MUYU_BUILD_TAG_HEADER */
