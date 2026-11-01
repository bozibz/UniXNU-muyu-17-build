#!/bin/bash
# Collect the Android-side GPU/GMU state needed to see how the vendor driver powers GX.
# Run from the workspace root while Android is booted with Magisk root and adb connected.
# Writes logs/android-harvest-<timestamp>/ and prints the directory.
set -u
OUT="logs/android-harvest-$(date +%Y%m%d-%H%M%S)"
mkdir -p "$OUT"

SU="/debug_ramdisk/su -c"
sh() { adb shell "$SU" "$1" 2>&1; }

adb wait-for-device
echo "== model: $(adb shell getprop ro.product.model 2>&1)"
echo "== build: $(adb shell getprop ro.build.version.incremental 2>&1)"

sh 'mount -t debugfs debugfs /sys/kernel/debug 2>/dev/null; echo mounted'
sh 'dmesg' > "$OUT/dmesg.txt"
sh 'ls -l /sys/kernel/debug/kgsl/kgsl-3d0/' > "$OUT/kgsl-nodes.txt"
sh 'cat /sys/kernel/debug/kgsl/kgsl-3d0/gpu' > "$OUT/kgsl-gpu-state.txt"
sh 'cat /sys/kernel/debug/clk/clk_summary' > "$OUT/clk_summary.txt"
sh 'cat /sys/kernel/debug/pm_genpd/pm_genpd_summary' > "$OUT/pm_genpd.txt"
sh 'cat /sys/kernel/debug/regulator/regulator_summary' > "$OUT/regulator_summary.txt"
sh 'cat /proc/interrupts' > "$OUT/interrupts.txt"
sh 'ls /sys/module/kgsl/parameters/ && cat /sys/module/kgsl/parameters/*' > "$OUT/kgsl-params.txt"

# Trigger real GPU work, then capture again: GX only powers up when the GPU is used.
adb shell am start -a android.intent.action.VIEW -d https://example.com >/dev/null 2>&1
sleep 5
sh 'cat /sys/kernel/debug/kgsl/kgsl-3d0/gpu' > "$OUT/kgsl-gpu-state-busy.txt"
sh 'cat /proc/interrupts' > "$OUT/interrupts-busy.txt"
sh 'dmesg | tail -300' > "$OUT/dmesg-busy.txt"

shasum -a 256 "$OUT"/* > "$OUT/SHA256SUMS" 2>/dev/null || true
echo
echo "harvested into: $OUT"
ls -la "$OUT"
