# scripts/legacy-10063 — the 17.7.11 line's forked build helpers (kept, not used)

These three scripts shadowed same-named scripts of the UniXNU core checkout: the assembled build tree
copies the device repository's `scripts/` over the core's, so while they lived in `scripts/` every build
of this board used them instead of core's.

They are forks of the xnu-10063 helpers and they break the iPadOS 27 line in ways that are hard to read
from the error alone:

* `build-xnu-4k-baseline.sh` applies the fork and device patch sets itself and does not honour
  `UNIXNU_PATCH_LIST`, so a schema-2 build (which lets the core apply the ordered series) got the 17
  line's patches applied on top of xnu-12377 and failed in `osfmk/arm64/start.s` / `bsd/kern/kern_exec.c`.
* `prepare-xnu-source.py` fetches DriverKit headers straight from raw.githubusercontent.com, which times
  out on this host; core's version honours `UNIXNU_HEADER_MIRROR`.
* `prepare-storage-headers.py` predates `pinned_source()`, which `build-sheng-source-drivers.py` calls, so
  the 27 source-driver path died with `AttributeError`.

They are kept because they document how the 17.7.11 images were built (`fw/vendor_boot_nofs1.img` and
friends). Restore one by moving it back into `scripts/` if that line ever has to be rebuilt.

`build-arm-driver.py` is deliberately still in `scripts/`: it carries this board's arm64e object audit and
profile-derived deployment target, which the core copy does not have.
