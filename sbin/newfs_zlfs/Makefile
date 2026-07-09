#	$OpenBSD$

PROG=	newfs_zlfs
MAN=	newfs_zlfs.8

DPADD=	${LIBUTIL}
LDADD=	-lutil

# Build against the in-tree headers so the dkzone/zlfs ABI does not
# depend on installed includes during bring-up.
CPPFLAGS+=	-I${.CURDIR}/../../sys

.include <bsd.prog.mk>
