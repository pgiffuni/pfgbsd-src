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

#ifndef _SYS_NET_MODBUS_MODBUS_FC_H_
#define	_SYS_NET_MODBUS_MODBUS_FC_H_

#include <sys/param.h>
#include <sys/types.h>

#include <net/modbus/modbus.h>
#include <net/modbus/modbus_backend.h>
#include <net/modbus/modbus_pdu.h>

/*
 * Function codes implemented in this module.  Section numbers refer to the
 * MODBUS Application Protocol Specification V1.1b3.
 */
#define	MODBUS_FC_READ_COILS		0x01	/* 6.1 */
#define	MODBUS_FC_READ_DISCRETE_IN	0x02	/* 6.2 */
#define	MODBUS_FC_READ_HOLDING_REG	0x03	/* 6.3 */
#define	MODBUS_FC_READ_INPUT_REG	0x04	/* 6.4 */
#define	MODBUS_FC_WRITE_SINGLE_COIL	0x05	/* 6.5 */
#define	MODBUS_FC_WRITE_SINGLE_REG	0x06	/* 6.6 */
#define	MODBUS_FC_WRITE_MULTI_COIL	0x0f	/* 6.11 */
#define	MODBUS_FC_WRITE_MULTI_REG	0x10	/* 6.12 */
#define	MODBUS_FC_MASK_WRITE_REG	0x16	/* 6.16 */
#define	MODBUS_FC_READ_WRITE_MULTI_REG	0x17	/* 6.17 */

/* Quantity limits from the request diagrams of those sections. */
#define	MODBUS_MAX_READ_BITS		2000	/* 0x7d0, 6.1 and 6.2 */
#define	MODBUS_MAX_WRITE_BITS		1968	/* 0x7b0, 6.11 */
#define	MODBUS_MAX_READ_REGS		125	/* 0x7d, 6.3 and 6.4 */
#define	MODBUS_MAX_WRITE_REGS		123	/* 0x7b, 6.12 */
#define	MODBUS_MAX_RW_REGS		121	/* 0x79, 6.17 */

/*
 * Execute one request PDU against a backend and produce the response PDU.
 *
 * Returns MODBUS_EX_OK with a normal response in "rsp", or an exception code
 * with an exception response already built in "rsp".  It never returns a
 * kernel errno: internal failures are mapped to MODBUS_EX_DEVICE_FAILURE.
 */
int modbus_fc_dispatch(struct modbus_backend *be,
    const struct modbus_pdu *req, struct modbus_pdu_buf *rsp);

#endif /* _SYS_NET_MODBUS_MODBUS_FC_H_ */
