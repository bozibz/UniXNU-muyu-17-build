#!/usr/bin/env python3
"""Put boot-progress probes into a COPY of the source XNU kernel (never the original).

There is no serial console on the device and the kernel cannot draw before its
own video console exists, so the early boot stages are reported by side
channels. The stage code lives in the zero padding after the kernel entry
(_start is one `b`) and displaced instructions are re-executed there:

  stage 1 (kernel entry, MMU off)       blue bar, then continues
  stage 2 (just before the MMU is on)   magenta bar, then continues
  stage 3 (right after the MMU is on)   waits --reset-delay seconds, then PSCI
                                        SYSTEM_RESET. A reboot by itself after
                                        that time shows the CPU got through the
                                        MMU enable; the frame buffer is not
                                        mapped by then, so time is the signal.
  panic screen (--panic-screen)         panic() and its common panic funnel draw
                                        their format string on the
                                        kernel's own frame buffer mapping and
                                        stops (scripts/xnu-panic-screen.c).
                                        --no-reset leaves out stages 3 and 4.
  stage 4 (--reset-after SYMBOL)        the same wait and reset, but placed after
                                        the call to SYMBOL in --in FUNCTION (the
                                        Nth call, --occurrence). It replaces
                                        stage 3, so one run asks one question:
                                        did the kernel get past that call?

The bars are the 1/16th bands of the frame buffer below the green one that
DarwinBoot paints (band index = stage). Each patch site is checked against its
expected original instruction first, and the tool refuses a kernel that does
not match. Offsets are specific to the kernel built by build-xnu-4k-baseline.sh.
"""
import argparse
import os
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

ENTRY_VA = 0xFFFFFFF00B9D0488        # LC_UNIXTHREAD pc, `b start_first_cpu`
START_FIRST_CPU = 0xFFFFFFF00B9D205C
MMU_ENABLE_VA = 0xFFFFFFF00B9D2330   # msr SCTLR_EL1, x0
AFTER_MMU_VA = 0xFFFFFFF00B9D2350    # mov x0, #0x300000
PANIC_VA = 0xFFFFFFF00BF69B04      # _panic
VINFO_VA = 0xFFFFFFF00C0A2970      # _vinfo
PANIC_CAVE_END = 0xFFFFFFF00B9D0FF0
CAVE_SIZE = 0x60
CAVES = {1: 0xFFFFFFF00B9D0500, 2: 0xFFFFFFF00B9D0560, 3: 0xFFFFFFF00B9D05C0, 4: 0xFFFFFFF00B9D0620}
TIMEBASE = 19200000

PAINT = """
  ldr x1, [{args}, #0x28]
  ldr x2, [{args}, #0x38]
  ldr x3, [{args}, #0x48]
  mul x2, x2, x3
  lsr x2, x2, #4
  bic x2, x2, #7
  mov x4, #{band}
  mul x5, x2, x4
  add x1, x1, x5
  movz w6, #{lo}
  movk w6, #{hi}, lsl #16
1:
  stp w6, w6, [x1], #8
  subs x2, x2, #8
  b.hi 1b
  dsb sy
"""

SOURCES = {
    1: ".text\n.global _start\n_start:\n" + PAINT.format(args='x0', band=1, lo='0x00FF', hi='0xFF00') + "  b target\n",
    2: ".text\n.global _start\n_start:\n" + PAINT.format(args='x20', band=2, lo='0x00FF', hi='0xFFFF')
       + "  msr SCTLR_EL1, x0\n  b target\n",
    3: """.text
.global _start
_start:
  mov x0, #0x300000
  mrs x1, cntpct_el0
  ldr x2, ={ticks}
  add x2, x1, x2
1:
  isb
  mrs x1, cntpct_el0
  cmp x1, x2
  b.lo 1b
  movz w0, #0x0009
  movk w0, #0x8400, lsl #16
  mov x1, xzr
  mov x2, xzr
  mov x3, xzr
  smc #0
2:
  wfe
  b 2b
""" + ".ltorg\n",
    4: """.text
.global _start
_start:
  bl callee
  mrs x1, cntpct_el0
  ldr x2, ={ticks}
  add x2, x1, x2
1:
  isb
  mrs x1, cntpct_el0
  cmp x1, x2
  b.lo 1b
  movz w0, #0x0009
  movk w0, #0x8400, lsl #16
  mov x1, xzr
  mov x2, xzr
  mov x3, xzr
  smc #0
2:
  wfe
  b 2b
""" + ".ltorg\n",
}

RESET_BEFORE = """.text
.global _start
_start:
  movz w0, #0x0009
  movk w0, #0x8400, lsl #16
  mov x1, xzr
  mov x2, xzr
  mov x3, xzr
  smc #0
1:
  wfe
  b 1b
"""

# Draw one line straight on the kernel's frame buffer through the panic screen
# renderer (it owns the font and the vinfo/VA handling), then park. Unlike the
# video console this bypasses gc_enabled/gc_desire_text, so it answers "can the
# kernel write the panel at all?" without depending on the console state.
FB_PROBE = """.text
.global _start
_start:
  ldr x0, =1f
  bl {panic}
2:
  wfe
  b 2b
1:
  .asciz "{text}"
.ltorg
"""

# Paint the whole frame buffer at kernel entry (MMU off, x0 = boot_args, so the
# GOP physical base from Video[0] is used directly) and park. The strongest
# available answer to "does the kernel run at all, and can it reach the panel?":
# it needs no mapping, no vinfo and no console.
FB_FILL = """.text
.global _start
_start:
{prologue}
  bic x2, x2, #7
  movz w6, #{lo}
  movk w6, #{hi}, lsl #16
1:
  stp w6, w6, [x1], #8
  subs x2, x2, #8
  b.hi 1b
  dsb sy
2:
  wfe
  b 2b
.ltorg
"""


def kernel_symbols(kernel):
    """Address -> name for the kernel's symbols, from llvm-nm."""
    nm = shutil.which('llvm-nm') or '/opt/homebrew/opt/llvm/bin/llvm-nm'
    table = {}
    for line in subprocess.run([nm, '-n', kernel], capture_output=True, text=True, check=True).stdout.splitlines():
        parts = line.split()
        if len(parts) != 3:
            continue
        # A fileset kernel prints one "<entry>:" / "Symbols:" banner per member, and those split into
        # three columns too, so only a hexadecimal first field is a symbol line.
        try:
            address = int(parts[0], 16)
        except ValueError:
            continue
        table.setdefault(address, parts[2])
    return table


def function_range(symbols, name):
    """Start and end (next symbol) of a function by its symbol name."""
    starts = sorted(symbols)
    for start, symbol in symbols.items():
        if symbol == name:
            return start, next(address for address in starts if address > start)
    raise SystemExit(f'No symbol {name} in the kernel')


def find_call(data, symbols, function, name, occurrence):
    """The VA and target of the Nth `bl name` inside FUNCTION."""
    hits = []
    low, high = function_range(symbols, function)
    for va in range(low, high, 4):
        word = struct.unpack_from('<I', data, file_offset(va))[0]
        if word >> 26 == 0b100101:
            delta = word & 0x3FFFFFF
            delta -= (1 << 26) if delta & (1 << 25) else 0
            target = va + delta * 4
            if symbols.get(target) == name:
                hits.append((va, target))
    if len(hits) < occurrence:
        raise SystemExit(f'{function} has {len(hits)} call(s) to {name}, not {occurrence}')
    return hits[occurrence - 1]


def assemble_c(source, va, workdir, symbols, trace_frames=0):
    """Build a freestanding C file for VA; returns (bytes, load address of the first byte, address of panic_screen).

    The linker may start .text a little after VA (it keeps the ELF headers in front), so the
    load address is read back from the ELF and is what the caller must write the bytes to."""
    clang = shutil.which('clang') or '/opt/homebrew/opt/llvm/bin/clang'
    lld = shutil.which('ld.lld') or '/opt/homebrew/opt/llvm/bin/ld.lld'
    objcopy = shutil.which('llvm-objcopy') or '/opt/homebrew/opt/llvm/bin/llvm-objcopy'
    nm = shutil.which('llvm-nm') or '/opt/homebrew/opt/llvm/bin/llvm-nm'
    # UNIXNU_UEFI is the Mu-Silicium checkout when the unixnu driver runs this script.
    font_dir = Path(os.environ.get('UNIXNU_UEFI') or ROOT / 'uefi-sheng') / 'Silicon/Silicium/SiliciumPkg/Library/FrameBufferSerialPortLib'
    if not (font_dir / 'Font.h').is_file():
        # The device repository vendors the same BSD-licensed font table, so a
        # payload build on Linux or a CI tree without a Mu-Silicium checkout
        # can still patch the panic screen.
        font_dir = ROOT / 'scripts'
    obj, elf, binary = (workdir / name for name in ('panic.o', 'panic.elf', 'panic.bin'))
    definitions = [f'-DPANIC_TRACE_FRAMES={trace_frames}']
    # Optional: the board's banner buffer for the panic message (kits/braveark/BootProgress.c); kernels without it get 0 (no copy).
    text = symbols.get('_boot_progress_panic_text')
    if text is not None:
        if not any(base <= text and text + 256 <= base + size for base, size, _, _ in SEGMENTS):
            raise SystemExit('Invalid _boot_progress_panic_text')
        definitions.append(f'-DPANIC_TEXT={text:#x}UL')
    # Optional: disableConsoleOutput, so the console (still drawn by the other CPUs onto the same
    # frame buffer) cannot overwrite the panic text after it is drawn. Ported from UniXNU-uke,
    # whose renderer is the same one this file is built from.
    quiet = symbols.get('_disableConsoleOutput')
    if quiet is not None:
        if quiet % 4 or not any(base <= quiet and quiet + 4 <= base + size for base, size, _, _ in SEGMENTS):
            raise SystemExit('Invalid _disableConsoleOutput')
        definitions.append(f'-DPANIC_QUIET={quiet:#x}UL')
    # Optional: the board's panic shadow and its length word, so a heartbeat thread can print the
    # message after the panic screen has parked the panicking CPU.
    shadow = symbols.get('_uke_panic_shadow')
    if shadow is not None and any(base <= shadow and shadow + 1024 <= base + size for base, size, _, _ in SEGMENTS):
        definitions.append(f'-DPANIC_SHADOW={shadow:#x}UL')
        length = symbols.get('_uke_panic_shadow_len')
        if length is not None and length % 4 == 0 and any(base <= length and length + 4 <= base + size for base, size, _, _ in SEGMENTS):
            definitions.append(f'-DPANIC_SHADOW_LEN={length:#x}UL')
    # Optional: the boot-log writer's pram staging, called from the panic screen because the normal
    # panic funnel is redirected to this screen.
    flush = symbols.get('_uke_panic_flush')
    if flush is not None:
        if flush % 4 or not any(base <= flush < base + size for base, size, _, _ in SEGMENTS):
            raise SystemExit('Invalid _uke_panic_flush')
        definitions.append(f'-DUKE_PANIC_FLUSH={flush:#x}UL')
    subprocess.run([clang, '--target=aarch64-none-elf', '-ffreestanding', '-fno-builtin', '-fno-stack-protector',
                    '-mgeneral-regs-only', '-mno-outline-atomics', '-fno-pic', '-mcmodel=tiny', '-Oz', f'-DVINFO={VINFO_VA:#x}UL',
                    *definitions, f'-I{font_dir}', '-c', source, '-o', obj], check=True)
    script = workdir / 'panic.ld'
    script.write_text('SECTIONS { . = %#x; .text : { *(.text.a_entry) *(.text.a_common) *(.text*) *(.rodata*) } /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) *(.ARM.*) } }\n' % va)
    size = shutil.which('llvm-size') or '/opt/homebrew/opt/llvm/bin/llvm-size'
    sizes = subprocess.run([size, '--format=berkeley', obj], capture_output=True, text=True, check=True).stdout.splitlines()[-1].split()
    if int(sizes[1]) or int(sizes[2]):
        # The code runs from the kernel's read-only text: a global would fault on its first store.
        raise SystemExit(f'{source.name} has writable data (data {sizes[1]}, bss {sizes[2]}); use the stack')
    subprocess.run([lld, '-T', script, '-e', 'panic_screen', obj, '-o', elf], check=True)
    # Only .text: a plain binary dump would start at the ELF headers kept in front of it.
    subprocess.run([objcopy, '-O', 'binary', '-j', '.text', elf, binary], check=True)
    readelf = shutil.which('llvm-readelf') or '/opt/homebrew/opt/llvm/bin/llvm-readelf'
    load = None
    for line in subprocess.run([readelf, '-S', elf], capture_output=True, text=True, check=True).stdout.splitlines():
        parts = line.replace('[', ' ').replace(']', ' ').split()
        if len(parts) >= 4 and parts[1] == '.text':
            load = int(parts[3], 16)
    if load is None or load < va:
        raise SystemExit('Cannot find where the linker put .text')
    entry = None
    for line in subprocess.run([nm, elf], capture_output=True, text=True, check=True).stdout.splitlines():
        parts = line.split()
        if len(parts) == 3 and parts[2] == 'panic_screen':
            entry = int(parts[0], 16)
    if entry != load:
        raise SystemExit(f'panic_screen at {entry:#x} is not the first byte {load:#x}')
    return binary.read_bytes(), load, entry


def panic_common_entry(workdir, code, load):
    """Resolve the real alternate entry and verify its distinct argument ABI."""
    nm = shutil.which('llvm-nm') or '/opt/homebrew/opt/llvm/bin/llvm-nm'
    names = {parts[2]: int(parts[0], 16)
             for line in subprocess.check_output([nm, workdir / 'panic.elf'], text=True).splitlines()
             if len(parts := line.split()) == 3}
    entry = names.get('panic_screen_common')
    main = names.get('panic_screen_main')
    if entry is None or main is None or not load <= entry <= load + len(code) - 8:
        raise SystemExit('Common panic renderer entry is missing or out of bounds')
    at = entry - load
    if code[at:at + 8] != struct.pack('<II', 0xf9400021, branch(entry + 4, main)):
        raise SystemExit('Common panic entry does not dereference the Darwin va_list pointer and branch to the renderer')
    return entry


ROOT = Path(__file__).resolve().parents[1]


SEGMENTS = []   # (vmaddr, vmsize, fileoff, filesize) of the kernel being patched


def load_segments(data):
    """Read the Mach-O segment table; the file layout differs between kernel builds."""
    magic, _, _, _, ncmds = struct.unpack_from('<IiiII', data, 0)
    if magic != 0xFEEDFACF:
        raise SystemExit('Not a 64-bit Mach-O')
    offset = 32
    SEGMENTS.clear()
    for _ in range(ncmds):
        cmd, size = struct.unpack_from('<II', data, offset)
        if cmd == 0x19:
            _, vmaddr, vmsize, fileoff, filesize = struct.unpack_from('<16sQQQQ', data, offset + 8)
            if filesize:
                SEGMENTS.append((vmaddr, vmsize, fileoff, filesize))
        offset += size


def unixthread_pc(data):
    """LC_UNIXTHREAD entry pc (arm64 thread state: flavor, count, x0..x28, fp, lr, sp, pc)."""
    offset = 32
    for _ in range(struct.unpack_from('<I', data, 16)[0]):
        cmd, size = struct.unpack_from('<II', data, offset)
        if cmd == 0x5:
            return struct.unpack_from('<Q', data, offset + 16 + 32 * 8)[0]
        offset += size
    raise SystemExit('No LC_UNIXTHREAD in kernel')


def rebase(kernel, data):
    """The probe sites are fixed offsets from the entry; shift them for another kernel build.

    _panic and _vinfo come from the symbol table when the kernel has one (the plain source
    kernel does); the retained candidate layout is the unshifted case.
    """
    global ENTRY_VA, START_FIRST_CPU, MMU_ENABLE_VA, AFTER_MMU_VA, PANIC_VA, VINFO_VA, PANIC_CAVE_END, CAVES
    delta = unixthread_pc(data) - ENTRY_VA
    if delta:
        ENTRY_VA += delta
        START_FIRST_CPU += delta
        MMU_ENABLE_VA += delta
        AFTER_MMU_VA += delta
        PANIC_CAVE_END += delta
        CAVES = {stage: va + delta for stage, va in CAVES.items()}
        PANIC_VA += delta
        VINFO_VA += delta
    names = {name: va for va, name in kernel_symbols(kernel).items()}
    if '_start_first_cpu' in names:
        START_FIRST_CPU = names['_start_first_cpu']
        # Native bootstrap table-capacity checks can grow before SCTLR setup.
        # Locate the unique instruction sequence in this bounded entry body;
        # do not assume a constant offset from the Mach-O entry point.
        matches = []
        for va in range(START_FIRST_CPU, START_FIRST_CPU + 0x1000, 4):
            def instruction(at):
                return struct.unpack_from('<I', data, file_offset(at))[0]
            if (instruction(va) == 0xD5181000 and
                    instruction(va + 4) == 0xD5033FDF and
                    instruction(va + 0x20) == 0xD2A00600 and
                    instruction(va + 0x24) == 0xD5181040):
                matches.append(va)
        if len(matches) != 1:
            raise SystemExit('Require one verified SCTLR/ISB/CPACR entry sequence')
        MMU_ENABLE_VA = matches[0]
        AFTER_MMU_VA = MMU_ENABLE_VA + 0x20
    if '_panic' in names and '_vinfo' in names:
        PANIC_VA = names['_panic']
        # Board diagnostics seed this ABI-compatible prefix from valid console
        # mappings, then pin it to their retained GOP mapping. Ordinary source
        # kernels without the observer retain the original vinfo path.
        VINFO_VA = names.get('_boot_progress_panic_vinfo', names['_vinfo'])
    return names


def file_offset(va):
    for vmaddr, vmsize, fileoff, filesize in SEGMENTS:
        if vmaddr <= va < vmaddr + min(vmsize, filesize):
            return fileoff + (va - vmaddr)
    raise SystemExit(f'{va:#x} is not in a file-backed segment of this kernel')


def branch(from_va, to_va):
    delta = (to_va - from_va) // 4
    if (to_va - from_va) % 4 or not -(1 << 25) <= delta < (1 << 25):
        raise SystemExit('Branch out of range')
    return 0x14000000 | (delta & 0x3FFFFFF)


def assemble(source, va, symbols, workdir):
    asm, obj, elf, binary = (workdir / name for name in ('cave.S', 'cave.o', 'cave.elf', 'cave.bin'))
    asm.write_text(source)
    clang = shutil.which('clang') or '/opt/homebrew/opt/llvm/bin/clang'
    lld = shutil.which('ld.lld') or '/opt/homebrew/opt/llvm/bin/ld.lld'
    objcopy = shutil.which('llvm-objcopy') or '/opt/homebrew/opt/llvm/bin/llvm-objcopy'
    subprocess.run([clang, '--target=aarch64-none-elf', '-c', asm, '-o', obj], check=True)
    subprocess.run([lld, f'-Ttext={va:#x}', *[f'--defsym={name}={value:#x}' for name, value in symbols.items()], '-e', '_start', obj, '-o', elf], check=True)
    subprocess.run([objcopy, '-O', 'binary', elf, binary], check=True)
    return binary.read_bytes()


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('kernel', type=Path, help='the COPY of the kernel to patch in place')
    parser.add_argument('--reset-delay', type=int, default=6, help='seconds before the reset')
    parser.add_argument('--reset-after', metavar='SYMBOL', help='place the reset after this call in arm_init instead of stage 3')
    parser.add_argument('--reset-before', action='store_true', help='reset at the call site before invoking SYMBOL (requires --reset-after)')
    parser.add_argument('--panic-screen', action='store_true', help='draw panic() messages on the screen')
    parser.add_argument('--no-reset', action='store_true', help='no reset probe at all')
    parser.add_argument('--fb-probe-after', metavar='SYMBOL',
                        help='after this call in --in, draw --fb-probe-text through the panic screen renderer and park (needs --panic-screen)')
    parser.add_argument('--fb-probe-text', default='FB PROBE: kernel writes the panel',
                        help='the line the --fb-probe-after probe draws (printable ASCII)')
    parser.add_argument('--fill-screen', type=lambda value: int(value, 0),
                        help='paint the whole frame buffer with this 0xAARRGGBB colour at kernel entry and park')
    parser.add_argument('--fill-base', type=lambda value: int(value, 0),
                        help='with --fill-screen: paint this physical address instead of boot_args Video[0]')
    parser.add_argument('--fill-size', metavar='WxH', type=lambda value: tuple(int(part, 0) for part in value.lower().split('x')),
                        help='with --fill-screen: paint this many pixels instead of the handed-over geometry')
    parser.add_argument('--occurrence', type=int, default=1, help='which call to SYMBOL (1 = first)')
    parser.add_argument('--in', dest='function', default='_arm_init', help='function that makes the call (default _arm_init)')
    args = parser.parse_args()
    if args.reset_before and not args.reset_after:
        raise SystemExit('--reset-before requires --reset-after')
    if args.fb_probe_after and (args.reset_after or not args.panic_screen):
        raise SystemExit('--fb-probe-after takes the stage 4 cave from --reset-after and needs --panic-screen')
    if args.fill_screen is not None and (args.fb_probe_after or args.reset_after):
        raise SystemExit('--fill-screen replaces every other probe')
    if any(c in args.fb_probe_text for c in '"\\\n') or not args.fb_probe_text.isascii():
        raise SystemExit('--fb-probe-text must be one line of printable ASCII without quotes')
    data = bytearray(args.kernel.read_bytes())
    load_segments(data)
    kernel_names = rebase(args.kernel, data)

    def word(va):
        return struct.unpack_from('<I', data, file_offset(va))[0]

    expected = {ENTRY_VA: branch(ENTRY_VA, START_FIRST_CPU), MMU_ENABLE_VA: 0xD5181000, AFTER_MMU_VA: 0xD2A00600}
    for va, value in expected.items():
        if word(va) != value:
            raise SystemExit(f'{va:#x} holds {word(va):#010x}, expected {value:#010x}: not the kernel these probes are for')
    for va in CAVES.values():
        if any(data[file_offset(va):file_offset(va) + CAVE_SIZE]):
            raise SystemExit(f'Padding at {va:#x} is not empty')

    # (site, cave stage, symbols the cave is linked with)
    # The frame-buffer probe parks right after PE_create_console, so the entry bars
    # would only be overwritten; dropping them leaves room for the panic screen.
    # The two entry bars are a boot-progress signal for a run that has nothing else to
    # report. A panic screen paints over them on its first clear, and they take the two
    # lowest caves, so a run that wants the panic screen and no reset/fb probe drops them
    # and gives the renderer 0x60 more bytes of padding.
    bars_wanted = not (args.panic_screen and args.no_reset and not args.fb_probe_after
                       and args.fill_screen is None)
    sites = [] if (args.fb_probe_after or args.fill_screen is not None or not bars_wanted) else [
        (ENTRY_VA, 1, {'target': START_FIRST_CPU}),
        (MMU_ENABLE_VA, 2, {'target': MMU_ENABLE_VA + 4})]
    sources = {}
    if args.fill_screen is not None:
        colour = args.fill_screen & 0xFFFFFFFF
        prologue = []
        if args.fill_base is not None:
            prologue.append(f'  ldr x1, ={args.fill_base:#x}')
        else:
            prologue.append('  ldr x1, [x0, #0x28]')
        if args.fill_size:
            width, height = args.fill_size
            prologue.append(f'  ldr x2, ={width * height * 4}')
            geometry = f'{width}x{height}'
        else:
            prologue += ['  ldr x2, [x0, #0x38]', '  ldr x3, [x0, #0x48]', '  mul x2, x2, x3']
            geometry = 'at the handed-over geometry'
        sites.append((ENTRY_VA, 1, {'target': START_FIRST_CPU}))
        sources[1] = FB_FILL.format(prologue='\n'.join(prologue), lo=colour & 0xFFFF, hi=(colour >> 16) & 0xFFFF)
        where = f'from {args.fill_base:#x}' if args.fill_base is not None else 'from boot_args Video[0]'
        description = f'whole-screen fill {colour:#010x} at kernel entry {where} {geometry}, then park'
    elif args.no_reset:
        description = 'no reset probe'
    elif args.reset_after:
        symbols = kernel_symbols(args.kernel)
        call_va, callee = find_call(data, symbols, args.function, args.reset_after, args.occurrence)
        sites.append((call_va, 4, {'callee': callee, 'target': call_va + 4}))
        description = f'stage 4 reset {"before" if args.reset_before else "after"} call #{args.occurrence} to {args.reset_after} in {args.function} (at {call_va:#x})'
    else:
        sites.append((AFTER_MMU_VA, 3, {'target': AFTER_MMU_VA + 4}))
        description = f'stage 3 reset {args.reset_delay} s after the MMU is on'
    if args.fb_probe_after:
        symbols = kernel_symbols(args.kernel)
        call_va, callee = find_call(data, symbols, args.function, args.fb_probe_after, args.occurrence)
        # Take the lowest free cave: the panic screen is placed right behind the last
        # cave in use and its own code needs the remaining padding.
        stage = next(s for s in (1, 2, 3, 4) if s not in {used for _, used, _ in sites})
        sites.append((call_va, stage, {'panic': PANIC_VA, 'target': call_va + 4}))
        sources[stage] = FB_PROBE.format(panic='panic', text=args.fb_probe_text)
        description += (f'; frame-buffer probe after call #{args.occurrence} to {args.fb_probe_after} in {args.function}'
                        f' (at {call_va:#x}): draws {args.fb_probe_text!r} through the panic screen and parks')

    with tempfile.TemporaryDirectory() as tmp:
        for site, stage, symbols in sites:
            source = (sources[stage] if stage in sources else
                      RESET_BEFORE if (stage == 4 and args.reset_before) else
                      SOURCES[stage].format(ticks=TIMEBASE * args.reset_delay))
            code = assemble(source, CAVES[stage], symbols, Path(tmp))
            if len(code) > CAVE_SIZE:
                raise SystemExit(f'Stage {stage} code is too large')
            data[file_offset(CAVES[stage]):file_offset(CAVES[stage]) + len(code)] = code
    for site, stage, _ in sites:
        struct.pack_into('<I', data, file_offset(site), branch(site, CAVES[stage]))
    if args.panic_screen:
        # Right after the last probe cave in use, so the padding is shared as tightly as possible.
        PANIC_CAVE = (max(CAVES[stage] for _, stage, _ in sites) + CAVE_SIZE) if sites else CAVES[1]
        if word(PANIC_VA) in (0, 0xFFFFFFFF) or any(data[file_offset(PANIC_CAVE):file_offset(PANIC_CAVE_END)]):
            raise SystemExit('panic() or the panic screen padding is not as expected')
        # The frame trace costs roughly a hundred bytes per link. Keep as many bounded
        # frame links as the kernel's padding holds, and drop the newest ones rather than
        # fail: the message plus its first frames is what a photographed panic is read for.
        built = None
        for frames in (6, 5, 4, 3, 2, 1, 0):
            with tempfile.TemporaryDirectory() as tmp:
                code, load, entry = assemble_c(ROOT / 'scripts/xnu-panic-screen.c', PANIC_CAVE,
                                               Path(tmp), kernel_names, trace_frames=frames)
                if load + len(code) <= PANIC_CAVE_END:
                    built = (code, load, entry, panic_common_entry(Path(tmp), code, load), frames)
                    break
        if built is None:
            raise SystemExit(f'panic screen does not fit the padding before {PANIC_CAVE_END:#x}')
        code, load, entry, common_entry, trace_frames = built
        if any(data[file_offset(load):file_offset(load) + len(code)]):
            raise SystemExit(f'Padding at {load:#x} is not empty')
        data[file_offset(load):file_offset(load) + len(code)] = code
        struct.pack_into('<I', data, file_offset(PANIC_VA), branch(PANIC_VA, entry))
        common = kernel_names.get('_panic_trap_to_debugger')
        if common is None or word(common) in (0, 0xFFFFFFFF):
            raise SystemExit('Common panic funnel is missing; refuse a partially covered panic screen')
        struct.pack_into('<I', data, file_offset(common), branch(common, common_entry))
        description += (f', panic screen ({len(code):#x} bytes at {entry:#x}; common funnel at {common:#x};'
                        f' {trace_frames} frame link(s))')
    args.kernel.write_bytes(data)
    bars = 'stage 1 blue bar, stage 2 magenta bar, ' if sites and not args.panic_screen else ''
    print(f'patched {args.kernel.name}: {bars}{description}')

if __name__ == '__main__':
    main()
