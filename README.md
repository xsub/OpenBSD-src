
# OpenBSD Zoned Block Device Experiments

Experimental OpenBSD source fork for native zoned block device support, focused on
host-managed SCSI ZBC / SMR disks and NVMe Zoned Namespaces.

This is research and bring-up work, not production-ready storage code.

## Goals

- Add a small native OpenBSD ABI for zoned block discovery.
- Support read-only zone capability queries and zone reports first.
- Keep host-managed devices safe until write ordering is implemented.
- Validate behavior with OpenBSD regress tests and QEMU-emulated zoned devices.
- Build toward real host-managed write support only after the read-only path is stable.

## Current Status

Implemented prototype pieces:

- `sys/sys/dkzone.h`
- `DIOCGZONEINFO`
- `DIOCGZONEREPORT`
- `sd(4)` recognition of SCSI ZBC host-managed devices
- SCSI `REPORT ZONES` translation into OpenBSD `struct dk_zone`
- read-only handling for host-managed zoned disks
- regression smoke test under `regress/sys/sys/dkzone`
- initial read-only NVMe ZNS reporting path
- QEMU/OpenBSD VM validation workflow

Tested so far:

- ABI regression builds and runs on OpenBSD/arm64.
- Non-zoned disk reports `zone_mode=0 flags=0x0`.
- Kernel boots on OpenBSD/arm64 VM.
- NVMe ZNS backend has passed syntax checks and needs full VM validation.

## Test Helper

The regression helper lives at:

```sh
regress/sys/sys/dkzone
```

Run:

```sh
cd /usr/src/regress/sys/sys/dkzone
make clean
make obj
make regress
./obj/dkzone /dev/rsd0c
```

On a normal non-zoned disk, expected output is:

```text
zone_mode=0 flags=0x0
```

On a zoned device, the helper should print zone capability data and a small
zone report.

## QEMU NVMe ZNS Target

QEMU 11 can expose an emulated NVMe Zoned Namespace. The current development VM
workflow uses an OpenBSD/arm64 guest on Apple Silicon with an optional second
NVMe ZNS test disk.

Example QEMU device shape:

```sh
-drive if=none,format=raw,file=openbsd-zns.raw,id=zns0
-device nvme,id=nvme-zns,serial=ZNS0001
-device nvme-ns,drive=zns0,nsid=1,zoned=on,zoned.zone_size=64M,zoned.zone_capacity=64M
```

Inside OpenBSD, inspect attachment with:

```sh
dmesg | grep -E 'nvme|sd[0-9]'
sysctl hw.disknames
```

## Roadmap

1. Stabilize read-only ABI.
2. Validate SCSI ZBC host-managed reporting against real or emulated devices.
3. Validate NVMe ZNS reporting under QEMU.
4. Add richer regress coverage for zone report layouts and option mapping.
5. Design safe write-path semantics for host-managed sequential zones.
6. Add zone reset/open/close/finish operations only after read-only reporting is reliable.
7. Evaluate filesystem and buffer-cache implications before enabling writable host-managed devices.

## Non-Goals For The Current Prototype

- No production write support yet.
- No filesystem-level zoned allocation policy yet.
- No promise of ABI stability before review.
- No attempt to support drive-managed SMR specially; those already appear as normal disks.

## Safety Model

Host-managed zoned devices are exposed conservatively. The first milestone is
discovery and reporting. Writable support must preserve sequential-write rules
before it can be enabled safely.

## References

- SCSI Zoned Block Commands, ZBC
- ATA Zoned Device ATA Commands, ZAC
- NVMe Zoned Namespaces, ZNS
- Linux zoned block layer
- libzbc
- libzbd
- QEMU NVMe ZNS emulation
```
