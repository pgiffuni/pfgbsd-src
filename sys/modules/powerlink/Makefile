#
# The openPOWERLINK protocol stack and its netgraph node, built as one KLD.
#
# The ported protocol sources are added as they are brought over; the FreeBSD
# platform layer, the driver interface, the real time engine and the node are
# written for FreeBSD and carry the BSD-2-Clause identifier instead.
#

.PATH:	${SRCTOP}/sys/netgraph
.PATH:	${SRCTOP}/sys/net/powerlink

KMOD=	powerlink
SRCS=	ng_powerlink.c				\
	powerlink_core.c			\
	powerlink_edrv_ng.c			\
	powerlink_rt.c				\
	powerlink_timer.c			\
	ng_powerlink.h				\
	powerlink.h				\
	powerlink_core.h			\
	powerlink_edrv.h			\
	powerlink_edrv_freebsd.h		\
	powerlink_edrv_ng.h			\
	powerlink_rt.h				\
	powerlink_timer.h			\
	opt_netgraph.h

.include <bsd.kmod.mk>