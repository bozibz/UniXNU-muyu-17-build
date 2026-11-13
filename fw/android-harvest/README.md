# Provenance

| file | source |
| --- | --- |
| msm-regs/a6xx.xml, a6xx_gmu.xml | mesa main (gitlab.freedesktop.org/mesa/mesa, src/freedreno/registers/adreno) — the GPU-offset numbering gen-muyu-gmu-source.py expects |
| msm-drv/a6xx_gmu.c | torvalds/linux v6.12 |
| msm-drv/a7xx_catalog.c | drm/msm msm-next (freedesktop gitlab) — keeps the inline `const struct a6xx_bcm[]` form and ADRENO_7XX_* families |
| vendor-source/arm-smmu-qcom.c | MiCode/Xiaomi_Kernel_OpenSource muyu-v-oss (the two-GPU-bank policy the generator pins) |
| ../stock/dtb_3.dtb | stock vendor_boot DTB section, entry 3 (qcom,cliffsp) of the OS3.0.307.0.WOYCNXM fastboot ROM |
| ../muyu_dt.txt | dtc -I dtb -O dts of the same DTB |
| ../gpu-firmware/gen71100_gmu.bin | vendor_a (EROFS) /firmware/gen71100_gmu.bin of the same ROM |

Validation: gen-muyu-gmu-source.py derives GMU fw chip id 0x070b0000, matching the
hardware-observed value in README.md.
