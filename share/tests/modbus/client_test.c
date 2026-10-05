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
 * Client transaction tests: the state machine that pairs one request with one
 * response.  No timer is involved, so these run on a host.
 */

#include <errno.h>
#include <string.h>

#include "modbus.h"
#include "modbus_client.h"

#include "test_util.h"

static void
test_client(void)
{
	struct modbus_client cl;
	const struct modbus_pdu_buf *rsp;
	const uint8_t *req;
	uint16_t len, unit, tid;

	modbus_client_reset(&cl);
	CHECK(cl.state == MODBUS_CLIENT_IDLE, "a fresh client is idle");

	/* A request is handed out exactly as supplied. */
	CHECK(modbus_client_start(&cl, (const uint8_t *)"\x03\x00\x10\x00\x02",
	    5, 0x11, 0x4321) == 0, "client accepts a request");
	req = modbus_client_request(&cl, &len, &unit, &tid);
	CHECK(req != NULL && len == 5 && unit == 0x11 && tid == 0x4321,
	    "client reports the request, unit and transaction identifier");

	/* Section 4.5: nothing may be started while one is outstanding. */
	CHECK(modbus_client_start(&cl, (const uint8_t *)"\x03", 1, 1, 1) ==
	    EBUSY, "client refuses a second concurrent request");
	CHECK(modbus_client_response(&cl) == NULL,
	    "no response is available before one arrives");

	/* A response for a different function code is not ours. */
	CHECK(modbus_client_input(&cl, (const uint8_t *)"\x04\x02\x00\x01", 4)
	    == MODBUS_CLIENT_IDLE, "client ignores another function code");
	CHECK(modbus_client_request(&cl, &len, &unit, &tid) != NULL,
	    "the request is still outstanding after a foreign response");

	/* A normal response completes the transaction. */
	CHECK(modbus_client_input(&cl, (const uint8_t *)"\x03\x04\x00\x11\x22\x33",
	    6) == MODBUS_CLIENT_DONE, "client accepts the matching response");
	rsp = modbus_client_response(&cl);
	CHECK(rsp != NULL && rsp->len == 6, "client keeps the response PDU");
	if (rsp != NULL && rsp->len == 6)
		CHECK(rsp->buf[0] == 0x03 && rsp->buf[1] == 0x04 &&
		    rsp->buf[2] == 0x00 && rsp->buf[3] == 0x11,
		    "client response holds the read data");

	CHECK(modbus_client_request(&cl, &len, &unit, &tid) == NULL,
	    "nothing is outstanding after a response");

	/* A second response after completion is ignored. */
	CHECK(modbus_client_input(&cl, (const uint8_t *)"\x03\x04\x00\x11\x22\x33",
	    6) == MODBUS_CLIENT_IDLE, "client ignores a duplicate response");

	/* An exception response is also a completed transaction. */
	CHECK(modbus_client_start(&cl, (const uint8_t *)"\x03\x00\x20\x00\x01",
	    5, 0x11, 0x4322) == 0, "client accepts a second request");
	CHECK(modbus_client_input(&cl, (const uint8_t *)"\x83\x02", 2) ==
	    MODBUS_CLIENT_DONE, "client accepts an exception response");
	rsp = modbus_client_response(&cl);
	CHECK(rsp != NULL && rsp->len == 2 && rsp->buf[0] == 0x83 &&
	    rsp->buf[1] == MODBUS_EX_ILLEGAL_ADDRESS,
	    "client keeps the exception code");

	/* A timeout leaves the client usable again. */
	CHECK(modbus_client_start(&cl, (const uint8_t *)"\x03\x00\x30\x00\x01",
	    5, 0x11, 0x4323) == 0, "client accepts a third request");
	CHECK(modbus_client_expired(&cl) == MODBUS_CLIENT_EXPIRED,
	    "client reports an expired transaction");
	CHECK(modbus_client_response(&cl) == NULL,
	    "an expired transaction has no response");
	CHECK(modbus_client_expired(&cl) == MODBUS_CLIENT_IDLE,
	    "expiring twice is harmless");

	/* A late response after the timeout must not resurrect it. */
	CHECK(modbus_client_input(&cl, (const uint8_t *)"\x03\x02\x00\x01", 4) ==
	    MODBUS_CLIENT_IDLE, "a late response is discarded");

	CHECK(modbus_client_start(&cl, (const uint8_t *)"\x03\x00\x40\x00\x01",
	    5, 0x11, 0x4324) == 0, "client accepts a fourth request");
	modbus_client_reset(&cl);
	CHECK(modbus_client_request(&cl, &len, &unit, &tid) == NULL,
	    "reset abandons the outstanding request");

	/* Malformed input to the transaction state is refused. */
	CHECK(modbus_client_start(&cl, NULL, 0, 1, 1) == EINVAL,
	    "client refuses a request with no PDU");
}

void
client_tests(void)
{

	test_client();
}
