#!/usr/bin/env python3
"""Compile the generic platform expert against the actual source-kernel ABI."""
import argparse
import json
import os
from pathlib import Path
import struct
import subprocess

ROOT = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('component', choices=['ArmVirtPlatform', 'ArmDTPlatform', 'ArmCpu', 'SoftwareAES', 'VirtioMmioBlock', 'RamBlock', 'GopFramebuffer', 'VirtioMmioConsole', 'NativeLegacyDisplay', 'NativeGOPPresenter', 'NativeOfflineGPU', 'NativeOfflineAccelerator', 'NativeIOGPUProviderProbe', 'ShengUfs', 'ShengHidInput', 'AppleKeyStoreState', 'Adreno740'])
parser.add_argument('--negative-aes-kat', action='store_true', help='Isolated own-source wrong-answer refusal control; never changes original inputs')
parser.add_argument('--verified-root', action='store_true', help='Authenticate all actual disk reads against an independently signed immutable policy')
parser.add_argument('--verified-root-bundle', action='store_true', help='Select exactly one capacity-matching policy per device, then independently authenticate it')
parser.add_argument('--negative-verified-block', action='store_true', help='Negative own-buffer control: corrupt only the completed DMA snapshot')
parser.add_argument('--unverified', action='store_true', help='ShengUfs: compile the bring-up transport without the verified-root policy bundle')
args = parser.parse_args()
if args.verified_root_bundle and not args.verified_root:
    parser.error('A policy bundle requires verified block mode')
if args.verified_root_bundle and args.negative_verified_block:
    parser.error('Select either bundle policy qualification or the isolated DMA snapshot fault')
if (args.verified_root or args.negative_verified_block) and args.component != 'VirtioMmioBlock':
    parser.error('Verified block mode requires the concrete read-only block transport')
if args.negative_verified_block and not args.verified_root:
    parser.error('The isolated input fault requires verified block mode')
if args.unverified and args.component != 'ShengUfs':
    parser.error('--unverified currently applies to the ShengUfs bring-up transport only')
component = args.component
recipe = ROOT / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/iokit/DEVELOPMENT/IOPlatformExpert.cpo.json'
record = json.loads(recipe.read_text().strip().rstrip(','))
source = ROOT / 'drivers' / ('GopFramebuffer' if component in ('NativeLegacyDisplay','NativeGOPPresenter') else 'AdrenoMetal' if component in ('NativeOfflineGPU','NativeOfflineAccelerator','NativeIOGPUProviderProbe') else component) / {'Adreno740':'NativeAdrenoService.cpp','SoftwareAES':'NativeAES.cpp','RamBlock':'NativeRamBlock.cpp','VirtioMmioBlock':'NativeVirtioBlock.cpp','VirtioMmioConsole':'NativeVirtioConsole.cpp','ShengUfs':'NativeUfs.cpp','AppleKeyStoreState':'NativeKeyStoreEvents.cpp','ShengHidInput':'NativeHidInput.cpp'}.get(component, component + '.cpp')
out = ROOT / 'build' / {'ArmVirtPlatform': 'armvirt-platform', 'ArmDTPlatform': 'armdt-platform', 'ArmCpu': 'armcpu', 'SoftwareAES': 'native-aes',
                        'VirtioMmioBlock': 'native-virtio-block', 'RamBlock': 'native-ram-block',
                        'GopFramebuffer': 'native-mobile-display', 'VirtioMmioConsole': 'native-virtio-console',
                        'NativeLegacyDisplay': 'native-legacy-display', 'NativeGOPPresenter': 'native-gop-presenter', 'NativeOfflineGPU':'native-offline-gpu','NativeOfflineAccelerator':'native-offline-accelerator','NativeIOGPUProviderProbe':'native-original-provider', 'ShengUfs':'native-ufs', 'ShengHidInput':'native-hid-input', 'AppleKeyStoreState':'native-keystore-events', 'Adreno740':'native-adreno'}[component]
if args.verified_root:
    out = ROOT / ('build/native-virtio-block-verified-fault' if args.negative_verified_block else 'build/native-virtio-block-verified')
    if args.verified_root_bundle:
        out = ROOT / 'build/native-virtio-block-verified-bundle'
out.mkdir(parents=True, exist_ok=True)
if args.negative_aes_kat:
    if component != 'SoftwareAES':
        parser.error('The own-source KAT control requires SoftwareAES')
    out = ROOT / 'build/native-aes-kat-negative'
    out.mkdir(parents=True, exist_ok=True)
    text = source.read_text()
    marker = '0x76,0x49,0xab,0xac'
    if text.count(marker) != 1:
        raise SystemExit('The exact own-source NIST answer was not uniquely identified')
    (out / 'OriginalAESABI.h').write_bytes(source.with_name('OriginalAESABI.h').read_bytes())
    source = out / 'SoftwareAES.cpp'
    source.write_text(text.replace(marker, '0x77,0x49,0xab,0xac'))
destination = out / (component + '.o')
arguments = record['arguments']
command = []
index = 0
while index < len(arguments):
    value = arguments[index]
    if value == record['file']:
        command.append(str(source))
    elif value == '-o':
        command.extend(['-o', str(destination)])
        index += 1
    elif value == '-Xclang' and arguments[index + 1].startswith('-W'):
        index += 1
    elif not value.startswith('-W'):
        command.append(value)
    index += 1
command.extend(['-Wall', '-Wextra', '-Werror', '-Wno-incompatible-sysroot', '-Wno-unused-parameter',
                '-Wno-c99-designator', '-Wno-reorder-init-list', '-Wno-deprecated-enum-enum-conversion',
                '-Wno-unnecessary-virtual-specifier', '-I', str(ROOT / '.cache/xnu-work/osfmk')])
if component in ('VirtioMmioBlock', 'RamBlock', 'ShengUfs'):
    command += ['-I', str(ROOT / 'build/storage-abi/include')]
    if args.verified_root:
        command += ['-I', str(ROOT / 'kits/VerifiedRoot/include'), '-DNATIVE_VERIFIED_BLOCK=1']
    if args.negative_verified_block:
        command += ['-DNATIVE_VERIFIED_BLOCK_INJECT_FAULT=1']
    if args.verified_root_bundle:
        command += ['-DNATIVE_VERIFIED_BLOCK_MULTI_POLICY=1']
if component == 'ShengUfs':
    command += ['-I', str(ROOT / 'kits/VerifiedRoot/include'), '-I', str(ROOT / 'drivers/VirtioMmioBlock'),
                '-mgeneral-regs-only']
    if not args.unverified:
        command += ['-DNATIVE_VERIFIED_BLOCK=1', '-DNATIVE_VERIFIED_BLOCK_MULTI_POLICY=1']
if component=='NativeOfflineGPU':
    command += ['-I',str(ROOT/'drivers/VirtioMmioConsole')]
if component in ('NativeLegacyDisplay','ShengHidInput'):
    command += ['-I', str(out)]
if component == 'Adreno740':
    command += ['-I', str(ROOT / 'kits/ArmSmmu4K'), '-I', str(ROOT / 'kits/AdrenoPM4'),
                '-I', str(ROOT / 'kits/braveark'), '-I', str(ROOT / 'kits/AdrenoFirmware'),
                '-I', str(ROOT / 'build/sheng-gmu')]
(out / 'compile-command.json').write_text(json.dumps(command, indent=2) + '\n')
result = subprocess.run(command, cwd=record['directory'], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
(out / 'compile.log').write_text(result.stdout)
if result.returncode:
    raise SystemExit(result.stdout[-3500:])
data = destination.read_bytes()
magic, cpu, subtype, kind, count, size = struct.unpack_from('<6I', data)
# arm64 is subtype 0, arm64e is subtype 2; the selected OS profile decides which one this build produces.
ARM64E_SUBTYPE = 2
wanted_subtype = ARM64E_SUBTYPE if os.environ.get('UNIXNU_XNU_ARCH') == 'arm64e' else 0
# arm64e objects carry the pointer-auth ABI bits in the subtype's high byte (0xC0000002 on this build,
# i.e. CPU_SUBTYPE_ARM64E | PTRAUTH_ABI | a kernel flag), so only the low byte names the base ABI.
if (magic, cpu, subtype & 0x00ffffff, kind) != (0xfeedfacf, 0x100000c, wanted_subtype, 1):
    raise SystemExit(f'Unexpected platform object architecture {cpu:#x}/{subtype} '
                     f'(wanted subtype {wanted_subtype} for {os.environ.get("UNIXNU_XNU_ARCH", "arm64")})')
offset = 32
deployment = None
for _ in range(count):
    cmd, length = struct.unpack_from('<II', data, offset)
    if cmd == 0x32:
        deployment = struct.unpack_from('<II', data, offset + 8)
    offset += length
# The deployment target comes from the OS profile (-miphoneos-version-min=X.Y in the recipe), so it is read
# back from the recorded command instead of being pinned to iOS 17.
# The OS profile is authoritative; the recorded recipe can be older than the tree it was copied into.
minimum = os.environ.get('UNIXNU_IOS_VERSION') or next(
    (argument.split('=', 1)[1] for argument in command
     if argument.startswith('-miphoneos-version-min=')), '17.0')
major, _, minor = minimum.partition('.')
wanted_deployment = (2, (int(major) << 16) | (int(minor or 0) << 8))
if deployment != wanted_deployment:
    raise SystemExit(f'Unexpected platform deployment {deployment} (wanted {wanted_deployment} '
                     f'from -miphoneos-version-min={minimum})')
archive = out / ('lib' + component + '.a')
archive.unlink(missing_ok=True)
subprocess.run(['xcrun', 'ar', 'rcs', archive, destination], check=True)
print(f'{component}: -Werror build and actual ARM64/iOS 17 object audit passed')
