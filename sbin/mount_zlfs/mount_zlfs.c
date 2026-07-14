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
#include <sys/mount.h>

#include <err.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "mntopts.h"

const struct mntopt mopts[] = {
	MOPT_STDOPTS,
	{ NULL }
};

static void __dead	usage(void);

int
main(int argc, char *argv[])
{
	struct zlfs_args args;
	const char *errstr;
	int ch, mntflags, sb_cap = 0;
	char *dev, *opt, *opts, *rest, dir[PATH_MAX];

	mntflags = 0;
	while ((ch = getopt(argc, argv, "o:")) != -1) {
		switch (ch) {
		case 'o':
			/*
			 * Handle the ZLFS-specific sbcap=N (test clamp on
			 * superblock-zone capacity) here; hand everything
			 * else to getmntopts.
			 */
			if ((opts = strdup(optarg)) == NULL)
				err(1, "strdup");
			for (rest = opts; (opt = strsep(&rest, ",")) != NULL;) {
				if (strncmp(opt, "sbcap=", 6) == 0) {
					sb_cap = strtonum(opt + 6, 1, 16384,
					    &errstr);
					if (errstr != NULL)
						errx(1, "sbcap is %s: %s",
						    errstr, opt + 6);
				} else if (*opt != '\0') {
					getmntopts(opt, mopts, &mntflags);
				}
			}
			free(opts);
			break;
		default:
			usage();
		}
	}
	argc -= optind;
	argv += optind;

	if (argc != 2)
		usage();

	dev = argv[0];
	if (realpath(argv[1], dir) == NULL)
		err(1, "realpath %s", argv[1]);

#define DEFAULT_ROOTUID	-2
	memset(&args, 0, sizeof(args));
	args.fspec = dev;
	args.export_info.ex_root = DEFAULT_ROOTUID;
	args.za_sb_cap = sb_cap;

	/* Read-write by default; -o ro selects a read-only mount. */
	if (mntflags & MNT_RDONLY)
		args.export_info.ex_flags = MNT_EXRDONLY;

	if (mount(MOUNT_ZLFS, dir, mntflags, &args) == -1)
		err(1, "mount %s on %s", dev, dir);

	return 0;
}

static void __dead
usage(void)
{
	fprintf(stderr, "usage: %s [-o options[,sbcap=N]] special node\n",
	    getprogname());
	exit(1);
}
