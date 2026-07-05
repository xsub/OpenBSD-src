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

#ifdef __OpenBSD__
#include <sys/ioctl.h>

#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#endif

static int
test_abi(void)
{
	struct dk_zone_info info = { 0 };
	struct dk_zone_report report = { 0 };
	struct dk_zone_op op = { 0 };
	struct dk_zone zones[2] = { { 0 } };

	if (DK_ZONE_VERSION != 1)
		return __LINE__;
	if (DIOCGZONEINFO == DIOCGZONEREPORT)
		return __LINE__;
	if (DIOCGZONEREPORT == DIOCZONECMD)
		return __LINE__;
	if (sizeof(info.dzi_flags) != sizeof(u_int64_t))
		return __LINE__;
	if (sizeof(zones[0].dz_start_lba) != sizeof(u_int64_t))
		return __LINE__;
	if (sizeof(report.dzr_zones) != sizeof(struct dk_zone *))
		return __LINE__;
	if (DK_ZONE_WP_INVALID != (u_int64_t)-1)
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

	op.dzo_version = DK_ZONE_VERSION;
	op.dzo_op = DK_ZONE_OP_RESET;
	op.dzo_lba = zones[0].dz_start_lba;
	op.dzo_flags = DK_ZONE_OP_F_ALL;

	if (op.dzo_op != DK_ZONE_OP_RESET)
		return __LINE__;
	if ((op.dzo_flags & DK_ZONE_OP_F_ALL) == 0)
		return __LINE__;

	return 0;
}

#ifdef __OpenBSD__
#define DKZONE_DEFAULT_ENTRIES	32
#define DKZONE_MAX_ENTRIES	4096
#define DKZONE_DEFAULT_SECSIZE	512

static void
usage(void)
{
	fprintf(stderr,
	    "usage: dkzone [-h] [-p] [-r report] [-n entries] "
	    "[-s start-lba] [device]\n");
	fprintf(stderr,
	    "       dkzone -m open|close|finish|reset [-a | -l lba] device\n");
	fprintf(stderr,
	    "       dkzone -w [-s start-lba] device\n");
	fprintf(stderr,
	    "reports: all empty imp-open exp-open closed full readonly "
	    "offline reset non-seq non-wp\n");
}

static u_int64_t
parse_u64(const char *arg, const char *name)
{
	unsigned long long value;
	char *ep;

	errno = 0;
	value = strtoull(arg, &ep, 0);
	if (arg[0] == '\0' || arg[0] == '-' || *ep != '\0' ||
	    errno == ERANGE) {
		if (strncmp(arg, "/dev/", 5) == 0)
			errx(1, "missing numeric %s before device %s",
			    name, arg);
		errx(1, "invalid %s: %s", name, arg);
	}

	return value;
}

static u_int
parse_entries(const char *arg)
{
	u_int64_t value;

	value = parse_u64(arg, "entry count");
	if (value > DKZONE_MAX_ENTRIES)
		errx(1, "entry count must be between 0 and %u",
		    DKZONE_MAX_ENTRIES);

	return value;
}

static const char *
zone_mode_name(u_int32_t mode)
{
	switch (mode) {
	case DK_ZONE_MODE_NONE:
		return "none";
	case DK_ZONE_MODE_HOST_AWARE:
		return "host-aware";
	case DK_ZONE_MODE_DRIVE_MANAGED:
		return "drive-managed";
	case DK_ZONE_MODE_HOST_MANAGED:
		return "host-managed";
	default:
		return "unknown";
	}
}

static const char *
zone_type_name(u_int32_t type)
{
	switch (type) {
	case DK_ZONE_TYPE_UNKNOWN:
		return "unknown";
	case DK_ZONE_TYPE_CONVENTIONAL:
		return "conventional";
	case DK_ZONE_TYPE_SEQ_REQUIRED:
		return "seq-required";
	case DK_ZONE_TYPE_SEQ_PREFERRED:
		return "seq-preferred";
	default:
		return "unknown";
	}
}

static const char *
zone_condition_name(u_int32_t condition)
{
	switch (condition) {
	case DK_ZONE_COND_UNKNOWN:
		return "unknown";
	case DK_ZONE_COND_NOT_WP:
		return "not-wp";
	case DK_ZONE_COND_EMPTY:
		return "empty";
	case DK_ZONE_COND_IMPLICIT_OPEN:
		return "implicit-open";
	case DK_ZONE_COND_EXPLICIT_OPEN:
		return "explicit-open";
	case DK_ZONE_COND_CLOSED:
		return "closed";
	case DK_ZONE_COND_READONLY:
		return "readonly";
	case DK_ZONE_COND_FULL:
		return "full";
	case DK_ZONE_COND_OFFLINE:
		return "offline";
	default:
		return "unknown";
	}
}

static const char *
zone_same_name(u_int32_t same)
{
	switch (same) {
	case DK_ZONE_SAME_ALL_DIFFERENT:
		return "all-different";
	case DK_ZONE_SAME_ALL_SAME:
		return "all-same";
	case DK_ZONE_SAME_LAST_DIFFERENT:
		return "last-different";
	case DK_ZONE_SAME_TYPES_DIFFERENT:
		return "types-different";
	default:
		return "unknown";
	}
}

static const char *
zone_report_name(u_int32_t option)
{
	switch (option) {
	case DK_ZONE_REP_ALL:
		return "all";
	case DK_ZONE_REP_EMPTY:
		return "empty";
	case DK_ZONE_REP_IMP_OPEN:
		return "imp-open";
	case DK_ZONE_REP_EXP_OPEN:
		return "exp-open";
	case DK_ZONE_REP_CLOSED:
		return "closed";
	case DK_ZONE_REP_FULL:
		return "full";
	case DK_ZONE_REP_READONLY:
		return "readonly";
	case DK_ZONE_REP_OFFLINE:
		return "offline";
	case DK_ZONE_REP_RESET:
		return "reset";
	case DK_ZONE_REP_NON_SEQ:
		return "non-seq";
	case DK_ZONE_REP_NON_WP:
		return "non-wp";
	default:
		return "unknown";
	}
}

static u_int32_t
parse_zone_report(const char *arg)
{
	if (strcmp(arg, "all") == 0)
		return DK_ZONE_REP_ALL;
	if (strcmp(arg, "empty") == 0)
		return DK_ZONE_REP_EMPTY;
	if (strcmp(arg, "imp-open") == 0 ||
	    strcmp(arg, "implicit-open") == 0)
		return DK_ZONE_REP_IMP_OPEN;
	if (strcmp(arg, "exp-open") == 0 ||
	    strcmp(arg, "explicit-open") == 0)
		return DK_ZONE_REP_EXP_OPEN;
	if (strcmp(arg, "closed") == 0)
		return DK_ZONE_REP_CLOSED;
	if (strcmp(arg, "full") == 0)
		return DK_ZONE_REP_FULL;
	if (strcmp(arg, "readonly") == 0 ||
	    strcmp(arg, "read-only") == 0)
		return DK_ZONE_REP_READONLY;
	if (strcmp(arg, "offline") == 0)
		return DK_ZONE_REP_OFFLINE;
	if (strcmp(arg, "reset") == 0)
		return DK_ZONE_REP_RESET;
	if (strcmp(arg, "non-seq") == 0 ||
	    strcmp(arg, "nonseq") == 0)
		return DK_ZONE_REP_NON_SEQ;
	if (strcmp(arg, "non-wp") == 0 ||
	    strcmp(arg, "nonwp") == 0)
		return DK_ZONE_REP_NON_WP;

	errx(1, "invalid zone report: %s", arg);
	return 0;
}

static const char *
zone_op_name(u_int32_t op)
{
	switch (op) {
	case DK_ZONE_OP_CLOSE:
		return "close";
	case DK_ZONE_OP_FINISH:
		return "finish";
	case DK_ZONE_OP_OPEN:
		return "open";
	case DK_ZONE_OP_RESET:
		return "reset";
	default:
		return "unknown";
	}
}

static u_int32_t
parse_zone_op(const char *arg)
{
	if (strcmp(arg, "close") == 0)
		return DK_ZONE_OP_CLOSE;
	if (strcmp(arg, "finish") == 0)
		return DK_ZONE_OP_FINISH;
	if (strcmp(arg, "open") == 0)
		return DK_ZONE_OP_OPEN;
	if (strcmp(arg, "reset") == 0)
		return DK_ZONE_OP_RESET;

	errx(1, "invalid zone command: %s", arg);
	return 0;
}

static void
print_zone(const struct dk_zone *zone, u_int64_t index)
{
	printf("%5llu %20llu %20llu %20llu %20llu %-15s %-15s "
	    "0x%08x 0x%08x 0x%08x 0x%08x\n",
	    (unsigned long long)index,
	    (unsigned long long)zone->dz_start_lba,
	    (unsigned long long)zone->dz_length_lba,
	    (unsigned long long)zone->dz_capacity_lba,
	    (unsigned long long)zone->dz_write_pointer_lba,
	    zone_type_name(zone->dz_type),
	    zone_condition_name(zone->dz_condition),
	    zone->dz_flags,
	    zone->dz_type_raw,
	    zone->dz_condition_raw,
	    zone->dz_flags_raw);
}

static void
print_zone_header(void)
{
	printf("%5s %20s %20s %20s %20s %-15s %-15s "
	    "%10s %10s %10s %10s\n",
	    "idx", "start_lba", "length_lba", "capacity_lba",
	    "wp_lba", "type", "condition", "flags", "raw_type",
	    "raw_cond", "raw_flags");
}

static void
test_device(const char *path, u_int64_t start_lba, u_int entries,
    u_int32_t report_option, int paginate)
{
	struct dk_zone_info info = { 0 };
	struct dk_zone_report report = { 0 };
	struct dk_zone *zones;
	struct dk_zone *last;
	u_int i;
	u_int64_t index = 0;
	u_int64_t next_lba;
	int header = 0;
	int fd;

	if (paginate && entries == 0)
		errx(1, "-p requires a non-zero entry count");

	zones = NULL;
	if (entries != 0) {
		zones = calloc(entries, sizeof(*zones));
		if (zones == NULL)
			err(1, "calloc");
	}

	fd = open(path, O_RDONLY);
	if (fd == -1)
		err(1, "open %s", path);

	if (ioctl(fd, DIOCGZONEINFO, &info) == -1)
		err(1, "DIOCGZONEINFO %s", path);

	printf("zone_mode=%u (%s) flags=0x%llx zone_size_lba=%llu\n",
	    info.dzi_zone_mode, zone_mode_name(info.dzi_zone_mode),
	    (unsigned long long)info.dzi_flags,
	    (unsigned long long)info.dzi_zone_size_lba);
	printf("max_open=%llu max_active=%llu optimal_open=%llu "
	    "optimal_nonseq=%llu max_seq=%llu\n",
	    (unsigned long long)info.dzi_max_open_zones,
	    (unsigned long long)info.dzi_max_active_zones,
	    (unsigned long long)info.dzi_optimal_open_zones,
	    (unsigned long long)info.dzi_optimal_nonseq_zones,
	    (unsigned long long)info.dzi_max_seq_zones);

	if (info.dzi_zone_mode == DK_ZONE_MODE_NONE) {
		free(zones);
		close(fd);
		return;
	}

	for (;;) {
		if (entries != 0)
			memset(zones, 0, entries * sizeof(*zones));
		memset(&report, 0, sizeof(report));
		report.dzr_version = DK_ZONE_VERSION;
		report.dzr_report_option = report_option;
		report.dzr_start_lba = start_lba;
		report.dzr_entries = entries;
		report.dzr_zones = zones;

		if (ioctl(fd, DIOCGZONEREPORT, &report) == -1)
			err(1, "DIOCGZONEREPORT %s", path);
		if (report.dzr_entries_filled > entries)
			errx(1, "driver returned too many zone descriptors");

		printf("report=%u (%s) same=%u (%s) start_lba=%llu max_lba=%llu "
		    "entries_filled=%u entries_available=%u\n",
		    report.dzr_report_option,
		    zone_report_name(report.dzr_report_option),
		    report.dzr_same, zone_same_name(report.dzr_same),
		    (unsigned long long)report.dzr_start_lba,
		    (unsigned long long)report.dzr_max_lba,
		    report.dzr_entries_filled, report.dzr_entries_available);

		if (report.dzr_entries_filled == 0)
			break;
		if (!header) {
			print_zone_header();
			header = 1;
		}
		for (i = 0; i < report.dzr_entries_filled; i++)
			print_zone(&zones[i], index++);

		if (!paginate)
			break;

		last = &zones[report.dzr_entries_filled - 1];
		next_lba = last->dz_start_lba + last->dz_length_lba;
		if (last->dz_length_lba == 0 ||
		    next_lba < last->dz_start_lba || next_lba <= start_lba)
			errx(1, "zone report did not advance");
		if (next_lba > report.dzr_max_lba)
			break;
		start_lba = next_lba;
	}

	free(zones);
	close(fd);
}

static void
zone_command(const char *path, u_int32_t op, u_int64_t lba, int all)
{
	struct dk_zone_op dzo = { 0 };
	int fd;

	fd = open(path, O_RDWR);
	if (fd == -1)
		err(1, "open %s", path);

	dzo.dzo_version = DK_ZONE_VERSION;
	dzo.dzo_op = op;
	dzo.dzo_lba = lba;
	if (all)
		dzo.dzo_flags = DK_ZONE_OP_F_ALL;

	if (ioctl(fd, DIOCZONECMD, &dzo) == -1)
		err(1, "DIOCZONECMD %s", path);

	printf("zone_%s lba=%llu flags=0x%llx ok\n", zone_op_name(op),
	    (unsigned long long)lba, (unsigned long long)dzo.dzo_flags);

	close(fd);
}

static void
write_probe(const char *path, u_int64_t lba)
{
	struct disklabel dl = { 0 };
	unsigned char *buf;
	u_int secsize = DKZONE_DEFAULT_SECSIZE;
	u_int64_t offset64;
	ssize_t n;
	int error;
	int fd;

	fd = open(path, O_RDWR);
	if (fd == -1)
		err(1, "open %s", path);

	if (ioctl(fd, DIOCGDINFO, &dl) != -1 && dl.d_secsize != 0)
		secsize = dl.d_secsize;

	if (lba > (u_int64_t)LLONG_MAX / secsize)
		errx(1, "write probe LBA is too large");
	offset64 = lba * secsize;

	buf = calloc(1, secsize);
	if (buf == NULL)
		err(1, "calloc");

	if (lseek(fd, (off_t)offset64, SEEK_SET) == -1)
		err(1, "lseek %s", path);

	n = write(fd, buf, secsize);
	error = errno;

	free(buf);
	close(fd);

	if (n == -1 && error == EROFS) {
		printf("ordinary_write lba=%llu error=EROFS ok\n",
		    (unsigned long long)lba);
		return;
	}
	if (n == -1) {
		errno = error;
		err(1, "ordinary write %s", path);
	}

	errx(1, "ordinary write unexpectedly wrote %zd bytes", n);
}
#endif

int
main(int argc, char **argv)
{
#ifdef __OpenBSD__
	const char *cmd = NULL;
	u_int entries = DKZONE_DEFAULT_ENTRIES;
	u_int32_t op = 0;
	u_int32_t report_option = DK_ZONE_REP_ALL;
	u_int64_t cmd_lba = 0;
	u_int64_t start_lba = 0;
	int all = 0;
	int ch, had_args;
	int have_lba = 0;
	int have_report_option = 0;
	int paginate = 0;
	int write_policy = 0;
#endif
	int error;

	error = test_abi();
	if (error != 0)
		return error;

#ifdef __OpenBSD__
	had_args = argc > 1;
	if (argc == 2 && strcmp(argv[1], "--help") == 0) {
		usage();
		return 0;
	}
	while ((ch = getopt(argc, argv, "ahl:m:n:pr:s:w")) != -1) {
		switch (ch) {
		case 'a':
			all = 1;
			break;
		case 'h':
			usage();
			return 0;
		case 'l':
			cmd_lba = parse_u64(optarg, "zone command LBA");
			have_lba = 1;
			break;
		case 'm':
			cmd = optarg;
			op = parse_zone_op(cmd);
			break;
		case 'n':
			entries = parse_entries(optarg);
			break;
		case 'p':
			paginate = 1;
			break;
		case 'r':
			report_option = parse_zone_report(optarg);
			have_report_option = 1;
			break;
		case 's':
			start_lba = parse_u64(optarg, "start LBA");
			break;
		case 'w':
			write_policy = 1;
			break;
		default:
			usage();
			return 1;
		}
	}
	argc -= optind;
	argv += optind;

	if (write_policy) {
		if (argc != 1) {
			usage();
			return 1;
		}
		if (cmd != NULL || all || have_lba || have_report_option ||
		    paginate)
			errx(1, "-w cannot be combined with -a, -l, -m, "
			    "-p, or -r");
		write_probe(argv[0], start_lba);
	} else if (cmd != NULL) {
		if (argc != 1) {
			usage();
			return 1;
		}
		if (!all && !have_lba)
			errx(1, "zone command requires -l lba unless -a is set");
		if (all && have_lba && cmd_lba != 0)
			errx(1, "-a requires omitted or zero -l lba");
		if (paginate)
			errx(1, "-p is only valid for zone reports");
		if (have_report_option)
			errx(1, "-r is only valid for zone reports");
		zone_command(argv[0], op, cmd_lba, all);
	} else if (argc == 1)
		test_device(argv[0], start_lba, entries, report_option,
		    paginate);
	else if (had_args) {
		usage();
		return 1;
	}
#else
	(void)argc;
	(void)argv;
#endif

	return 0;
}
