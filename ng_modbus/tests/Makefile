#
# Host build of the Modbus protocol tests.
#
# The protocol core under sys/net/modbus is written for the kernel, so this
# build compiles it against the small shim in shim/ that supplies the few
# kernel facilities it uses.  The result is a plain userland binary that
# exercises the PDU layer, the function code handlers, the memory backend and
# exception mapping with no netgraph node, no netgraph stack and no hardware.
#
# The netgraph node itself is not built here; it is compiled as part of
# modbus.ko (see sys/modules/modbus/Makefile).
#

CC?=	clang
# Named separately so an inherited CFLAGS from the environment cannot
# silently drop the language level or the warnings.
TESTCFLAGS=	-std=gnu17 -Wall -Wextra -Wno-unused-parameter -O -g

# Absolute, because the generated symlink below points into the tree and a
# relative path would depend on how deep the tests directory sits.
SRCTOP?=	../..
SRCTOP:=	${.CURDIR}/${SRCTOP}
MODBUS=		${SRCTOP}/sys/net/modbus

PROG=		modbus_test
SRCS=		modbus_test.c	\
		framing_test.c	\
		client_test.c	\
		shim_malloc.c	\
		${MODBUS}/modbus.c		\
		${MODBUS}/modbus_pdu.c		\
		${MODBUS}/modbus_fc.c		\
		${MODBUS}/modbus_crc.c		\
		${MODBUS}/modbus_rtu.c		\
		${MODBUS}/modbus_ascii.c		\
		${MODBUS}/modbus_tcp.c		\
		${MODBUS}/modbus_client.c	\
		${MODBUS}/modbus_backend.c

CLEANFILES=	${PROG}

# The shim must come first so that <sys/types.h> and friends resolve to it
# rather than to the kernel headers.  The protocol headers are reached through
# a generated symlink: shipping one would tie the test build to a relative
# path in this tree.
TESTCFLAGS+=	-I shim

all: ${PROG}

shim/net/modbus:
	@mkdir -p shim/net
	@ln -sf ${MODBUS} shim/net/modbus

${PROG}: ${SRCS} shim/net/modbus
	${CC} ${TESTCFLAGS} -o ${PROG} ${SRCS}

run: ${PROG}
	./${PROG}

clean:
	rm -f ${CLEANFILES} shim/net/modbus

.PHONY: all run clean