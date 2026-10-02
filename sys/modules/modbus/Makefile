#
# The Modbus protocol engine and its netgraph node, built as one KLD.
#
# $Whistle: Makefile,v 1.3 1999/01/19 23:46:16 archie Exp $
#

.PATH:	${SRCTOP}/sys/netgraph
.PATH:	${SRCTOP}/sys/net/modbus

KMOD=	modbus
SRCS=	ng_modbus.c				\
	modbus.c				\
	modbus_pdu.c				\
	modbus_fc.c				\
	modbus_client.c			\
	modbus_backend.c			\
	modbus_crc.c				\
	modbus_rtu.c				\
	modbus_ascii.c				\
	modbus_tcp.c				\
	modbus.h				\
	modbus_pdu.h				\
	modbus_fc.h				\
	modbus_client.h			\
	modbus_backend.h			\
	modbus_crc.h				\
	modbus_rtu.h				\
	modbus_ascii.h				\
	modbus_tcp.h				\
	opt_netgraph.h

.include <bsd.kmod.mk>