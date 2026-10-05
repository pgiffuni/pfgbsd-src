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

/*
 * Common engine entry point.  A transport hands over the bytes of one request
 * PDU and receives the bytes of one response PDU; whether those bytes came
 * from a TCP stream, an RS-485 line or a test harness is not visible here.
 */

#include <sys/param.h>
#include "modbus_sys.h"
#include <sys/errno.h>

#include "modbus.h"
#include "modbus_backend.h"
#include "modbus_fc.h"
#include "modbus_pdu.h"

/*
 * Handle one request PDU.
 *
 * Returns 0 with a normal response in "rsp", a positive Modbus exception
 * code with an exception response in "rsp", or a negative kernel errno when
 * the input is not a PDU at all and nothing may be sent back.  The three
 * cases never overlap: exception codes are 1..11 and errno values are not
 * returned as protocol responses.
 */
int
modbus_handle(const struct modbus_ctx *ctx, const uint8_t *p, size_t len,
    struct modbus_pdu_buf *rsp)
{
	struct modbus_pdu pdu;
	int error;

	modbus_pdu_buf_init(rsp);

	if (ctx == NULL || ctx->backend == NULL)
		return (-EINVAL);
	if ((error = modbus_pdu_parse(p, len, &pdu)) != 0)
		return (-error);

	error = modbus_fc_dispatch(ctx->backend, &pdu, rsp);

	return (error);
}
