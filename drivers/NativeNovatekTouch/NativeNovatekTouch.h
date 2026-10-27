// SPDX-License-Identifier: BSD-2-Clause-Patent
// The NT36532E touch driver service (NativeNovatekTouch.cpp). One entry point: the board brings the
// service up when muyu-touch-drv=1, exactly where the display probe used to spawn the raw pump thread.
#ifndef NATIVE_NOVATEK_TOUCH_H
#define NATIVE_NOVATEK_TOUCH_H
#include <IOKit/IOService.h>

// Returns the started service, or nullptr when muyu-touch-drv is absent or the part did not come up.
// The service owns the mappings, the DMA buffer, the NVT state and the pump thread; it stays in the
// IORegistry for the life of the boot.
extern "C" IOService *native_novatek_touch_start(void);
#endif
