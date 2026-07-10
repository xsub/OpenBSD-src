#	$OpenBSD$

PROG=	mount_zlfs
SRCS=	mount_zlfs.c getmntopts.c
MAN=	mount_zlfs.8

MOUNT=	${.CURDIR}/../mount
CFLAGS+= -I${MOUNT} -I${.CURDIR}/../../sys
.PATH:	${MOUNT}

.include <bsd.prog.mk>
