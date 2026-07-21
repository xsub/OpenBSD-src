#	$OpenBSD$

PROG=	fsck_zlfs
MAN=	fsck_zlfs.8

# Build against the in-tree headers so the zlfs ABI does not depend on
# installed includes during bring-up.
CPPFLAGS+=	-I${.CURDIR}/../../sys

.include <bsd.prog.mk>
