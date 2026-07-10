/*	$OpenBSD$	*/
/*
 * Copyright (c) 2026 Pawel Suchanecki <subdcc@gmail.com>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#ifndef _SYS_DKZONE_H_
#define _SYS_DKZONE_H_

#include <sys/types.h>

/*
 * User/kernel ABI for zoned disk devices.  Capability queries and zone reports
 * are read-only.  Zone management operations change device state and are kept
 * in a separate command structure so normal write support can be developed
 * independently.
 */
#define DK_ZONE_VERSION		1

struct dk_zone_info {
	u_int32_t	dzi_version;
	u_int32_t	dzi_zone_mode;
#define DK_ZONE_MODE_NONE		0x00
#define DK_ZONE_MODE_HOST_AWARE		0x01
#define DK_ZONE_MODE_DRIVE_MANAGED	0x02
#define DK_ZONE_MODE_HOST_MANAGED	0x04

	u_int64_t	dzi_flags;
#define DK_ZONE_FLAG_UNRESTRICTED_READ	0x00000001
#define DK_ZONE_FLAG_REPORT_SUP		0x00000002
#define DK_ZONE_FLAG_OPEN_SUP		0x00000004
#define DK_ZONE_FLAG_CLOSE_SUP		0x00000008
#define DK_ZONE_FLAG_FINISH_SUP		0x00000010
#define DK_ZONE_FLAG_RESET_SUP		0x00000020
#define DK_ZONE_FLAG_OPT_SEQ_VALID	0x00000040
#define DK_ZONE_FLAG_OPT_NONSEQ_VALID	0x00000080
#define DK_ZONE_FLAG_MAX_SEQ_VALID	0x00000100

	u_int64_t	dzi_zone_size_lba;
	u_int64_t	dzi_max_open_zones;
	u_int64_t	dzi_max_active_zones;
	u_int64_t	dzi_optimal_open_zones;
	u_int64_t	dzi_optimal_nonseq_zones;
	u_int64_t	dzi_max_seq_zones;
	u_int64_t	dzi_reserved[8];
};

struct dk_zone {
	u_int64_t	dz_start_lba;
	u_int64_t	dz_length_lba;
	u_int64_t	dz_capacity_lba;
	u_int64_t	dz_write_pointer_lba;
#define DK_ZONE_WP_INVALID		((u_int64_t)-1)

	u_int32_t	dz_type;
#define DK_ZONE_TYPE_UNKNOWN		0x00
#define DK_ZONE_TYPE_CONVENTIONAL	0x01
#define DK_ZONE_TYPE_SEQ_REQUIRED	0x02
#define DK_ZONE_TYPE_SEQ_PREFERRED	0x03

	u_int32_t	dz_condition;
#define DK_ZONE_COND_UNKNOWN		0x00
#define DK_ZONE_COND_NOT_WP		0x01
#define DK_ZONE_COND_EMPTY		0x02
#define DK_ZONE_COND_IMPLICIT_OPEN	0x03
#define DK_ZONE_COND_EXPLICIT_OPEN	0x04
#define DK_ZONE_COND_CLOSED		0x05
#define DK_ZONE_COND_READONLY		0x06
#define DK_ZONE_COND_FULL		0x07
#define DK_ZONE_COND_OFFLINE		0x08

	u_int32_t	dz_flags;
#define DK_ZONE_FLAG_RESET_RECOMMENDED	0x00000001
#define DK_ZONE_FLAG_NON_SEQ_RESOURCES	0x00000002

	/*
	 * Preserve protocol values so userland can reason about newer ZBC/ZAC
	 * values before the native enum is extended.
	 */
	u_int32_t	dz_type_raw;
	u_int32_t	dz_condition_raw;
	u_int32_t	dz_flags_raw;
	u_int32_t	dz_reserved32;
	u_int64_t	dz_reserved64[4];
};

struct dk_zone_report {
	u_int32_t	dzr_version;
	u_int32_t	dzr_report_option;
#define DK_ZONE_REP_ALL		0x00
#define DK_ZONE_REP_EMPTY	0x01
#define DK_ZONE_REP_IMP_OPEN	0x02
#define DK_ZONE_REP_EXP_OPEN	0x03
#define DK_ZONE_REP_CLOSED	0x04
#define DK_ZONE_REP_FULL	0x05
#define DK_ZONE_REP_READONLY	0x06
#define DK_ZONE_REP_OFFLINE	0x07
#define DK_ZONE_REP_RESET	0x10
#define DK_ZONE_REP_NON_SEQ	0x11
#define DK_ZONE_REP_NON_WP	0x3f

	u_int64_t	dzr_start_lba;
	u_int64_t	dzr_max_lba;

	u_int32_t	dzr_same;
#define DK_ZONE_SAME_ALL_DIFFERENT	0x00
#define DK_ZONE_SAME_ALL_SAME		0x01
#define DK_ZONE_SAME_LAST_DIFFERENT	0x02
#define DK_ZONE_SAME_TYPES_DIFFERENT	0x03

	/*
	 * dzr_entries is the size of the user output array.  A zero value
	 * requests a header-only report; dzr_zones may be NULL in that case.
	 */
	u_int32_t	dzr_entries;
	u_int32_t	dzr_entries_filled;
	/*
	 * Protocol-dependent count for the requested report.  This may
	 * describe only the current report page, not the total number of
	 * zones on the device.  Callers that enumerate zones should advance
	 * dzr_start_lba from the last returned descriptor.
	 */
	u_int32_t	dzr_entries_available;
	struct dk_zone	*dzr_zones;
	u_int64_t	dzr_reserved[8];
};

#ifdef _KERNEL
/*
 * In-kernel zone reporting for filesystem consumers.  Fills up to
 * nzones descriptors into the kernel array starting at start_lba,
 * paging through device reports internally; *filled receives the
 * number of descriptors written (fewer than nzones at end of device).
 * Unlike DIOCGZONEREPORT this never touches the sd(4) raw-write-gate
 * zone cache.  The device must be an sd(4) disk.
 */
int	dk_zone_report_kern(dev_t, u_int64_t, struct dk_zone *, u_int32_t,
	    u_int32_t *);
#endif /* _KERNEL */

struct dk_zone_op {
	u_int32_t	dzo_version;
	u_int32_t	dzo_op;
#define DK_ZONE_OP_CLOSE	0x01
#define DK_ZONE_OP_FINISH	0x02
#define DK_ZONE_OP_OPEN		0x03
#define DK_ZONE_OP_RESET	0x04

	/*
	 * Absolute device LBA for the zone to operate on.  If
	 * DK_ZONE_OP_F_ALL is set, dzo_lba must be zero and is ignored.
	 */
	u_int64_t	dzo_lba;

	u_int64_t	dzo_flags;
#define DK_ZONE_OP_F_ALL	0x00000001

	u_int64_t	dzo_reserved[8];
};

#endif /* _SYS_DKZONE_H_ */
