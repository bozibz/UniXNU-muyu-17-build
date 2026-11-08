# patches/xnu/backlog — written, never applied

These 16 patches are **not** part of the 27 line's build. `patches/xnu/series-spike27` does not
list them, and the framework applies only what that series lists. They sat in `patches/xnu/`
directly until 2026-11-08, where they looked exactly like live patches — that is why they were
moved here.

## What they are

They are the iOS 17 line's (BraveArk / 21H461) device patches, copied into this repo at some
point during the 27-line migration. Filenames and content match the 17-line tree:

| Patch | What it does on the 17 line |
| --- | --- |
| `offline-owner-root-auth-bypass.patch` | publish the owner root in unverified bring-up mode |
| `zzzz-offline-owner-systemos-bringup.patch` | SystemOS owner-cache bring-up on the 27 fileset |
| `zzzzz-video-console-physical-fb.patch` | add the low-bit "physical" marker to the GOP base |
| `zzzzzz-video-console-text.patch` | `-bravearktextconsole`: graphics boot → text console |
| `zzzzzzz-last-panic-print.patch` | paint the previous boot's pram log tail at `vcattach()` |
| `zzzzzzzz-braveark-watchdog-policy.patch` | keep `com.apple.watchdogd` from resetting the boot |
| `zzzzzzzzz-braveark-hold-reboot.patch` / `-hold-restart.patch` | park on the panic text instead of rebooting (a pair — both or neither) |
| `zzzzzzzzzzzz-muyu-console-quiet.patch` | stop drawing once the render server owns the scanout |
| `zzzzzzzzzzzzz-stackshot-tolerate-thread-count-drift.patch` | bounds-check stackshot's fill pass |
| `zzzzzzzzzzzzzz-shared-region-map-trace.patch` | trace `shared_region_check_np` |
| `zzzzzzzzzzzzzzz-critexit-reason.patch` | print init's critical-exit reason before the panic |
| `zzzzzzzzzzzzzzzz-spawn-mount-argv.patch` | print the argv launchd passes to `mount-phase-1` |
| `zzzzzzzzzzzzzzzzz-mount-syscall-trace.patch` | why `mount -P 1` returns ENOENT |
| `zzzzzzzzzzzzzzzzzz-exec-trace.patch` | which helper in the exec chain dies |
| `zzzzzzzzzzzzzzzzzzz-console-tail.patch` | mirror `/dev/console` output into the video console |

## Why they are here and not in the series

The 27 line never applied them. The 16:12 build of 2026-11-08 applied 72 patches — 67 from the
framework's `patches/xnu-12377` layer and 5 device patches — and none of the 16 above. That is
also why the 27 kernel has no `STACK64` page and no `muyu_vc_*` console: those patches were
copied in, but never wired. `docs/SCREEN-ONLY.md` §3 is the same finding from the binary side.

## When to take them back out

They are the raw material for the screen re-plant in `docs/SCREEN-ONLY.md` §4. When that work
starts, move the ones it needs back to `patches/xnu/` **and add them to `series-spike27` in the
same commit** — a patch that is present but unlisted is the exact failure this directory exists
to stop. Run `python3 scripts/audit-27-config.py` afterwards.

Note the naming: these carry the 17 line's `zzzz…` sort keys, which only mattered when the
loader globbed a directory. With an explicit series the prefix has no effect; rename to
something descriptive when they are wired in.
