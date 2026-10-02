/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Pedro Giffuni
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#ifndef _SYS_NET_MODBUS_H_
#define	_SYS_NET_MODBUS_H_

#include <sys/param.h>
#include <sys/types.h>

/*
 * The backend and PDU types live in their own headers and include this one,
 * so they are only forward declared here.
 */
struct modbus_backend;
struct modbus_pdu_buf;

/*
 * Transport independent Modbus definitions.
 *
 * Sizes and code values are taken from:
 *   MODBUS Application Protocol Specification V1.1b3 (26 Apr 2012),
 *   sections 4.1 (protocol description), 4.2 (data encoding) and
 *   7 (exception responses);
 *   MODBUS over Serial Line Specification and Implementation Guide V1.02
 *   (20 Dec 2006), sections 2.5.1.1 (RTU framing) and 2.5.2.1
 *   (ASCII framing).
 *
 * This header is transport neutral on purpose: nothing here may depend on
 * netgraph, on TCP, or on the serial line.
 */

/* PDU: function code (1 byte) plus function specific data (0..252). */
#define	MODBUS_PDU_MAXLEN	253
#define	MODBUS_DATA_MAXLEN	252

/* RTU ADU: address (1) + PDU (253) + CRC (2). */
#define	MODBUS_RTU_ADU_MAXLEN	256

/* TCP ADU: MBAP header (7) + PDU (253). */
#define	MODBUS_TCP_MBAPLEN	7
#define	MODBUS_TCP_ADU_MAXLEN	260

/* ASCII frame: ':' + address/data/LRC as hex pairs + CR + LF. */
#define	MODBUS_ASCII_FRAME_MAXLEN	513

/* Unit (slave) address ranges, Serial Line guide section 2.2. */
#define	MODBUS_UNIT_BROADCAST	0
#define	MODBUS_UNIT_MIN		1
#define	MODBUS_UNIT_MAX		247

/* Serial line unit addresses reserved by the specification. */
#define	MODBUS_UNIT_RESERVED_MIN	248
#define	MODBUS_UNIT_RESERVED_MAX	255

/* Function code handling, Application Protocol Specification section 4.1. */
#define	MODBUS_FC_MASK		0x80	/* set in a response on exception */
#define	MODBUS_FC_MIN		0x01	/* function code 0 is not valid */
#define	MODBUS_FC_MAX		0x7f	/* 0x80..0xff are exception codes */

/*
 * Modbus exception codes, Application Protocol Specification section 7.
 *
 * These are protocol values and are deliberately distinct from kernel
 * errno values.  The internal helpers may return an errno to the netgraph
 * layer, but nothing here may ever be derived from errno and nothing derived
 * from here may be reported as an errno.
 */
typedef enum {
	MODBUS_EX_OK			= 0x00,
	MODBUS_EX_ILLEGAL_FUNCTION	= 0x01,
	MODBUS_EX_ILLEGAL_ADDRESS	= 0x02,
	MODBUS_EX_ILLEGAL_VALUE		= 0x03,
	MODBUS_EX_DEVICE_FAILURE	= 0x04,
	MODBUS_EX_ACKNOWLEDGE		= 0x05,
	MODBUS_EX_DEVICE_BUSY		= 0x06,
	MODBUS_EX_NEGATIVE_ACKNOWLEDGE	= 0x07,
	MODBUS_EX_MEMORY_PARITY		= 0x08,
	MODBUS_EX_GATEWAY_TARGET	= 0x09,
	MODBUS_EX_GATEWAY_PATH		= 0x0a
} modbus_exc_t;

/* Protocol roles; on a serial line these are the master and the slave. */
#define	MODBUS_ROLE_SERVER	1
#define	MODBUS_ROLE_CLIENT	2

/* MODBUS data model, Application Protocol Specification section 4.3. */
typedef enum {
	MODBUS_AREA_COIL,		/* read/write: 01, 05, 15 */
	MODBUS_AREA_DISCRETE_IN,	/* read only:  02 */
	MODBUS_AREA_INPUT_REG,		/* read only:  04 */
	MODBUS_AREA_HOLDING_REG		/* read/write: 03, 06, 16, 22, 23 */
} modbus_area_t;

/*
 * Everything a Modbus endpoint needs, independent of how it is carried.
 * One of these lives in the netgraph node private data and is handed to
 * modbus_handle() by whichever transport decoded a request.
 */
struct modbus_ctx {
	struct modbus_backend	*backend;
	uint8_t			 unit_id;	/* our address */
	uint8_t			 role;		/* MODBUS_ROLE_* */
};

/*
 * Returns 0 with a response in "rsp", a positive Modbus exception code with
 * an exception response in "rsp", or a negative kernel errno when nothing may
 * be sent back.
 */
int modbus_handle(const struct modbus_ctx *ctx, const uint8_t *p, size_t len,
    struct modbus_pdu_buf *rsp);

#endif /* _SYS_NET_MODBUS_H_ */
