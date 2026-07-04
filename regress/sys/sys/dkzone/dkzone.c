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

#include <sys/types.h>
#include <sys/disklabel.h>
#include <sys/dkio.h>

int
main(void)
{
	struct dk_zone_info info = { 0 };
	struct dk_zone_report report = { 0 };
	struct dk_zone zones[2] = { { 0 } };

	if (DK_ZONE_VERSION != 1)
		return __LINE__;
	if (DIOCGZONEINFO == DIOCGZONEREPORT)
		return __LINE__;
	if (sizeof(info.dzi_flags) != sizeof(u_int64_t))
		return __LINE__;
	if (sizeof(zones[0].dz_start_lba) != sizeof(u_int64_t))
		return __LINE__;
	if (sizeof(report.dzr_zones) != sizeof(struct dk_zone *))
		return __LINE__;

	info.dzi_version = DK_ZONE_VERSION;
	info.dzi_zone_mode = DK_ZONE_MODE_HOST_MANAGED;
	info.dzi_flags = DK_ZONE_FLAG_REPORT_SUP | DK_ZONE_FLAG_RESET_SUP;

	if (info.dzi_zone_mode != DK_ZONE_MODE_HOST_MANAGED)
		return __LINE__;
	if ((info.dzi_flags & DK_ZONE_FLAG_REPORT_SUP) == 0)
		return __LINE__;

	zones[0].dz_start_lba = 4096;
	zones[0].dz_length_lba = 65536;
	zones[0].dz_type = DK_ZONE_TYPE_SEQ_REQUIRED;
	zones[0].dz_condition = DK_ZONE_COND_EMPTY;
	zones[0].dz_flags = DK_ZONE_FLAG_RESET_RECOMMENDED;

	report.dzr_version = DK_ZONE_VERSION;
	report.dzr_report_option = DK_ZONE_REP_ALL;
	report.dzr_start_lba = zones[0].dz_start_lba;
	report.dzr_entries = 2;
	report.dzr_zones = zones;

	if (report.dzr_zones[0].dz_type != DK_ZONE_TYPE_SEQ_REQUIRED)
		return __LINE__;
	if (report.dzr_report_option != DK_ZONE_REP_ALL)
		return __LINE__;

	return 0;
}
