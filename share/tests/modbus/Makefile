#
# Host build of the Modbus protocol tests.
#
# Run it with make: "make run".  This is a BSD make Makefile, and on a
# FreeBSD system make(1) is BSD make.  A GNU make on a host system will
# not understand it.
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

# The tree root, so this builds from any working directory and from a
# checkout anywhere.  Override it for an out of tree build, for example
# "make SRCTOP=/usr/src run".
SRCTOP?=	${.CURDIR}/../../..
MODBUS=		${SRCTOP}/sys/net/modbus

PROG=		modbus_test
SRCS=		modbus_test.c	\
		framing_test.c	\
		client_test.c	\
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

# The engine and these tests include the protocol headers with quotes, so the
# only include path needed is the directory holding them.  It contains no
# system headers, so it shadows nothing: a host build still gets the real
# <sys/param.h>, <sys/malloc.h> and the rest.
TESTCFLAGS+=	-I ${MODBUS}

all: ${PROG}

${PROG}: ${SRCS}
	${CC} ${TESTCFLAGS} -o ${PROG} ${SRCS}

run: ${PROG}
	./${PROG}

clean:
	rm -f ${CLEANFILES}

.PHONY: all run clean