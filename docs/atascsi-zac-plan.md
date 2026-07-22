# ATA ZAC (host-managed SMR SATA) support — parked branch `zlfs-f5-atascsi-zac`

**Status: implemented, compiles clean, adversarial review pending fold-in,
HARDWARE VALIDATION BLOCKED (no SATA ZAC in QEMU). Do not merge until the
WDC 26 TB drive validates on a real AHCI port.**

## Context

ZLFS's validation target is a WDC WSH722626ALE600 (26 TB Ultrastar,
`identifyDeviceType: SMR-HM`), engineering firmware mutated to 100% SMR —
zero conventional zones, exactly ZLFS's design case. The drive is **SATA
(ZAC)**. The fork's ZBD layer speaks SCSI ZBC + NVMe ZNS; `atascsi(4)`
had no zone support at all.

Two attach paths were considered:
- **Behind a SAS HBA** — the HBA's SATL presents the drive as SCSI
  `T_ZBC`, and the existing `sd(4)` ZBC path drives it unchanged. No
  kernel work (this is what phase f4/`zlfs-physmr.sh` assumes).
- **Direct AHCI** — the subject of this branch. OpenBSD does not
  translate ZBC↔ZAC, so a native SATL was needed inside `atascsi`.

This branch implements the second path.

## Architecture (decided, do not relitigate)

Translate SCSI ZBC CDBs to ATA ZAC commands **inside `atascsi`** (a SATL
layer). `sd(4)` and the SCSI midlayer are untouched — they already enable
the whole zoned block path (ioctls, `dk_zone` kernel API, the raw-write
gate) the moment INQUIRY reports peripheral type `T_ZBC`
(`sys/scsi/sd.c` `sd_zoned_params`). The `scsi_adapter` `ioctl` /
`dev_zone_report` hooks stay NULL; sd's native ZBC fallback loops
`ZBC_IN`/`ZBC_OUT` CDBs through `atascsi`'s `scsi_cmd`, which is the path
implemented here.

## What is implemented (5 files, ~316 lines)

- **`sys/dev/ata/atascsi.h`** — `SATA_SIGNATURE_ZAC 0xabcd0101`;
  `ATA_PORT_T_ZAC 4`; `ATA_C_ZAC_MGMT_IN 0x4a` / `ATA_C_ZAC_MGMT_OUT 0x9f`;
  `ATA_ZAC_IN_REPORT_ZONES 0x00`; `ATA_ZAC_OUT_{CLOSE,FINISH,OPEN,RESET_WP}`
  (1..4, matching the SCSI ZBC service-action values).
- **`sys/dev/ic/ahci.c`** — `ahci_port_signature()` gains a high-16 else-if
  returning `ATA_PORT_T_ZAC` (ZAC drives previously misclassified as
  `T_DISK`).
- **`sys/dev/ic/sili.c`** — both softreset signature switches add
  `case SATA_SIGNATURE_ZAC` (ZAC drives previously returned `T_NONE`, i.e.
  did not attach at all on sili); the PMP active-ports switch adds
  `ATA_PORT_T_ZAC` alongside DISK/ATAPI.
- **`sys/dev/ata/atascsi.c`**:
  - `ATA_PORT_F_ZAC` feature flag; `atascsi_probe` normalizes
    `ATA_PORT_T_ZAC` → `ap_type = T_DISK` + `SET(F_ZAC)` immediately, so
    every disk setup/dispatch path runs unchanged (IDENTIFY 0xEC, NCQ,
    write cache, lookahead).
  - TRIM advertisement gated off when `F_ZAC` (DSM/write-pointer
    interaction is device-specific and useless to the zoned path; also
    keeps UNMAP / WRITE SAME / thin VPD inert).
  - INQUIRY and all 7 VPD pages emit the device type via
    `atascsi_disk_device_type()` (`T_ZBC` when `F_ZAC`, else `T_DIRECT`).
  - `atascsi_disk_cmd` dispatches `ZBC_IN`/`ZBC_OUT`.
  - **`atascsi_disk_zbc_in`** (REPORT ZONES → ATA MGMT IN, DMA data-in):
    validates CDB, `dma_alloc(roundup(datalen,512))` bounce (`XS_BUSY` on
    NULL), FIS `0x4a` / feature `0x00` / options+PARTIAL in
    `features_exp` / LBA48 / page count in `sector_count`; the completion
    byte-swaps the little-endian ZAC report to the big-endian SCSI layout
    (`atascsi_zac_report_letoh`), copies `min(returned, datalen)` to
    `xs->data`, sets `xs->resid`, frees the bounce on every state.
  - **`atascsi_disk_zbc_out`** (CLOSE/FINISH/OPEN/RESET → ATA MGMT OUT,
    non-data): `feature = sa`, ALL bit in `features_exp`, LBA48 (0 when
    ALL); completion maps ATA states to `XS_*`.

Non-ZAC disks and ATAPI are byte-identical (every change gated on the ZAC
signature / `F_ZAC`, which no existing device produces). `sd(4)`/scsi
unchanged.

## Verification status

Local `clang -fsyntax-only` (incl. `-Werror=unused-variable,
-Werror=uninitialized, -Werror=implicit-function-declaration`) passes on
all three kernel files. An adversarial review round (5 lenses, MUST-VERIFY
ledger below) was launched; **fold its confirmed findings in here before
any hardware test.**

## MUST-VERIFY ledger (encodings not confirmable from the tree)

The tree has no prior ZAC constants; the register encodings below were
written from the ZAC-2/ACS-4 spec and Linux libata semantics from memory.
Each must be confirmed against the spec AND the drive before trusting a
write to it:

- **V1** — ZAC MGMT IN opcode `0x4a`, FEATURE(7:0)=`0x00` (REPORT ZONES),
  DMA data-in protocol.
- **V2** — reporting options + PARTIAL in FEATURE(15:8) (`features_exp`),
  PARTIAL = bit 7; copying SCSI `zone_options` straight is valid because
  the byte layouts coincide.
- **V3** — ZAC MGMT OUT opcode `0x9f`, FEATURE(7:0)=zone-mgmt action (SCSI
  SA values 1..4 == ATA action values), ALL bit in FEATURE(15:8) bit 0.
- **V4** — RETURN PAGE COUNT is a count of 512-byte pages in
  `sector_count`(15:0); behavior when the count is smaller than what the
  device would return.
- **V5** — ZAC REPORT ZONES header/descriptor multibyte fields are
  little-endian and share single-byte-field offsets with SCSI ZBC, so
  `atascsi_zac_report_letoh` is correct and complete (header `length` and
  `maximum_lba`; per-descriptor `zone_length`, `zone_start_lba`,
  `write_pointer_lba`).
- **V6** — host-managed signature `0xabcd0101` (LBA high `0xAB`, LBA mid
  `0xCD`).

## Remaining work (the TODO)

1. **Fold in the adversarial review findings** (round `wf_b76846d6-629`)
   — resolve any confirmed defect, especially anything in the MUST-VERIFY
   ledger.
2. **Confirm the MUST-VERIFY ledger against the ZAC-2 spec** (or Linux
   `drivers/ata/libata-scsi.c` `ata_scsi_zbc_in_xlat` /
   `ata_scsi_zbc_out_xlat` as a cross-check).
3. **Hardware validation** on the WDC 26 TB attached to a plain AHCI
   port — the only possible runtime evidence (QEMU has no SATA ZAC):
   - `dmesg`: drive attaches as `sd*` with `, zoned`; INQUIRY type is not
     the `T_DIRECT` fallback.
   - `dkzone -r all -n 8 -s 0 /dev/rsdXc` — read-only. Byte-swap or
     page-count errors show instantly as absurd zone sizes / start LBAs or
     short/empty reports.
   - zone ops on the FIRST zone only (open/close/finish/reset via
     `dkzone`), re-report after each, check condition/WP transitions; time
     a RESET ALL (watch the `sd` 300 s timeout — extend
     `sd_zbc_zone_cmd`'s value if a 26 TB reset-all exceeds it).
   - `regress/sys/sys/dkzone/zlfs-physmr.sh sdXc 32`, then the full suite
     with `ZLFS_NEWFS_ZONES=32`.
4. **Optional** — emit VPD page `0xb6` (`SI_PG_DISK_ZONED`) from a GPL log
   read so `sd_vpd_zoned` learns the optimal/max-open hints. Not required:
   sd already marks the disk HOST_MANAGED with the *_SUP flags before the
   VPD read, whose result it discards.

## Blast radius / honest caveats

- Touches `ahci` **and** `sili` (both classify SATA signatures); a ZAC
  drive on `sili` did not attach before this change.
- pciide/`wd(4)` is NOT covered — a ZAC drive on the legacy IDE stack gets
  no zone support (out of scope).
- `atascsi` is never instantiated in the QEMU dev VM (virtio+NVMe only),
  so this phase cannot regress VM runtime — but also cannot be validated
  there.
