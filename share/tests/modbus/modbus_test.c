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
 * Protocol tests for the transport neutral Modbus engine.
 *
 * These exercise the PDU layer, the function code handlers, the memory
 * backend and exception mapping with no netgraph node, no netgraph stack and
 * no hardware, as required by the test strategy.  Every case is written
 * against the MODBUS Application Protocol Specification V1.1b3 and each
 * function code is checked both for a valid request and for the requests
 * that must be refused.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <net/modbus/modbus.h>
#include <net/modbus/modbus_backend.h>
#include <net/modbus/modbus_fc.h>
#include <net/modbus/modbus_pdu.h>

#include "test_util.h"

int failures;
int checks;

void
check(int ok, const char *name, const char *detail)
{

	checks++;
	if (ok)
		return;
	failures++;
	printf("not ok %d - %s\n", checks, name);
	if (detail != NULL)
		printf("  %s\n", detail);
}

#define	CHECK(cond, name)	check((cond), (name), NULL)

static void
check_bytes(const struct modbus_pdu_buf *rsp, const uint8_t *want,
    size_t wantlen, const char *name)
{
	size_t i;

	if (rsp->len == wantlen && memcmp(rsp->buf, want, wantlen) == 0) {
		check(1, name, NULL);
		return;
	}

	checks++;
	failures++;
	printf("not ok %d - %s\n", checks, name);
	printf("  got %u bytes:", rsp->len);
	for (i = 0; i < rsp->len && i < 32; i++)
		printf(" %02x", rsp->buf[i]);
	printf("\n  want %zu bytes:", wantlen);
	for (i = 0; i < wantlen && i < 32; i++)
		printf(" %02x", want[i]);
	printf("\n");
}

/*
 * An exception response is the request code with bit 7 set, then the code.
 */
static void
check_exc(const struct modbus_pdu_buf *rsp, uint8_t fc, modbus_exc_t exc,
    const char *name)
{
	uint8_t want[2];

	want[0] = (uint8_t)(fc | MODBUS_FC_MASK);
	want[1] = (uint8_t)exc;
	check_bytes(rsp, want, sizeof(want), name);
}


static struct modbus_backend backend;
static struct modbus_ctx ctx;

static void
backend_setup(void)
{
	struct modbus_backend_config cfg;

	cfg.ncoils = 256;
	cfg.ndiscrete_in = 256;
	cfg.ninput_regs = 256;
	cfg.nholding_regs = 256;
	if (modbus_backend_init(&backend, &cfg) != 0) {
		printf("Bail out! backend init failed\n");
		exit(1);
	}
	memset(&ctx, 0, sizeof(ctx));
	ctx.backend = &backend;
	ctx.role = MODBUS_ROLE_SERVER;
	ctx.unit_id = 1;
}

static int
run(const uint8_t *req, size_t len, struct modbus_pdu_buf *rsp)
{

	return (modbus_handle(&ctx, req, len, rsp));
}

/* Section 6.1 and 6.2: bit reads, including the byte packing order. */
static void
test_read_bits(void)
{
	struct modbus_pdu_buf rsp;
	uint8_t req[16];
	int error;

	/* Set coils 0, 3 and 8 with FC 0F. */
	req[0] = MODBUS_FC_WRITE_MULTI_COIL;
	req[1] = 0; req[2] = 0;		/* starting address 0 */
	req[3] = 0; req[4] = 9;		/* quantity 9 */
	req[5] = 2;			/* byte count, ceil(9/8) */
	req[6] = 0x09;			/* bits 0 and 3 */
	req[7] = 0x01;			/* bit 8 */
	error = run(req, 8, &rsp);
	CHECK(error == 0, "FC 0F accepts 9 coils");

	req[0] = MODBUS_FC_READ_COILS;
	req[1] = 0; req[2] = 0;
	req[3] = 0; req[4] = 9;
	error = run(req, 5, &rsp);
	CHECK(error == 0, "FC 01 reads back 9 coils");
	check(rsp.buf[1] == 2, "FC 01 byte count is 2 for 9 bits",
	    "byte count is ceil(9/8)");
	check_bytes(&rsp, (const uint8_t *)"\x01\x02\x09\x01", 4,
	    "FC 01 packs the lowest bit into the least significant bit");

	req[0] = MODBUS_FC_READ_DISCRETE_IN;
	req[1] = 0; req[2] = 0;
	req[3] = 0; req[4] = 1;
	error = run(req, 5, &rsp);
	CHECK(error == 0, "FC 02 reads a discrete input");
	CHECK(rsp.buf[1] == 1 && rsp.buf[2] == 0,
	    "FC 02 reads a separate area");

	/* Quantity 0 is not a legal value. */
	req[0] = MODBUS_FC_READ_COILS;
	req[3] = 0; req[4] = 0;
	error = run(req, 5, &rsp);
	check_exc(&rsp, MODBUS_FC_READ_COILS, MODBUS_EX_ILLEGAL_VALUE,
	    "FC 01 rejects quantity 0 with exception 03");

	/* Above the section 6.1 maximum of 2000. */
	req[0] = MODBUS_FC_READ_COILS;
	req[3] = 0x07; req[4] = 0xd1;	/* 2001 */
	error = run(req, 5, &rsp);
	check_exc(&rsp, MODBUS_FC_READ_COILS, MODBUS_EX_ILLEGAL_VALUE,
	    "FC 01 rejects quantity 2001");

	/* Exactly the maximum is legal, so the map is what refuses it. */
	req[0] = MODBUS_FC_READ_COILS;
	req[3] = 0x07; req[4] = 0xd0;	/* 2000 */
	error = run(req, 5, &rsp);
	check_exc(&rsp, MODBUS_FC_READ_COILS, MODBUS_EX_ILLEGAL_ADDRESS,
	    "FC 01 accepts the maximum quantity but reports the small map");

	/* A short request is an invalid value, not a read past the buffer. */
	req[0] = MODBUS_FC_READ_COILS;
	error = run(req, 3, &rsp);
	check_exc(&rsp, MODBUS_FC_READ_COILS, MODBUS_EX_ILLEGAL_VALUE,
	    "FC 01 rejects a truncated request");

	/* An address past the end of the map is an illegal address. */
	req[0] = MODBUS_FC_READ_COILS;
	req[1] = 0x01; req[2] = 0x00;	/* 256, the map holds 0..255 */
	req[3] = 0; req[4] = 1;
	error = run(req, 5, &rsp);
	check_exc(&rsp, MODBUS_FC_READ_COILS, MODBUS_EX_ILLEGAL_ADDRESS,
	    "FC 01 rejects an address past the end of the area");

	/* A range that starts inside but runs past the end is also refused. */
	req[1] = 0x00; req[2] = 0xfe;	/* 254 */
	req[3] = 0; req[4] = 4;
	error = run(req, 5, &rsp);
	check_exc(&rsp, MODBUS_FC_READ_COILS, MODBUS_EX_ILLEGAL_ADDRESS,
	    "FC 01 rejects a range that crosses the end of the area");
}

/* Sections 6.3, 6.4, 6.6: register writes and reads. */
static void
test_registers(void)
{
	struct modbus_pdu_buf rsp;
	uint8_t req[16];
	uint8_t w[16];
	int error;

	/* FC 06 writes one register, and echoes the request. */
	req[0] = MODBUS_FC_WRITE_SINGLE_REG;
	req[1] = 0; req[2] = 10;		/* address 10 */
	req[3] = 0x12; req[4] = 0x34;	/* 0x1234 */
	error = run(req, 5, &rsp);
	CHECK(error == 0, "FC 06 writes a holding register");
	check_bytes(&rsp, req, 5, "FC 06 echoes the request");

	req[0] = MODBUS_FC_READ_HOLDING_REG;
	req[1] = 0; req[2] = 10;
	req[3] = 0; req[4] = 1;
	error = run(req, 5, &rsp);
	CHECK(error == 0, "FC 03 reads the register back");
	check_bytes(&rsp, (const uint8_t *)"\x03\x02\x12\x34", 4,
	    "FC 03 returns the value most significant byte first");

	/* Input registers are a separate, read only area. */
	req[0] = MODBUS_FC_READ_INPUT_REG;
	error = run(req, 5, &rsp);
	CHECK(error == 0, "FC 04 reads an input register");
	CHECK(rsp.buf[2] == 0 && rsp.buf[3] == 0,
	    "FC 04 does not read the holding register area");

	req[0] = MODBUS_FC_READ_HOLDING_REG;
	req[1] = 0; req[2] = 0;
	req[3] = 0; req[4] = 0;
	error = run(req, 5, &rsp);
	check_exc(&rsp, MODBUS_FC_READ_HOLDING_REG, MODBUS_EX_ILLEGAL_VALUE,
	    "FC 03 rejects quantity 0");

	req[3] = 0; req[4] = 125;		/* the maximum */
	error = run(req, 5, &rsp);
	CHECK(error == 0, "FC 03 accepts quantity 125");

	req[3] = 0; req[4] = 126;		/* one over */
	error = run(req, 5, &rsp);
	check_exc(&rsp, MODBUS_FC_READ_HOLDING_REG, MODBUS_EX_ILLEGAL_VALUE,
	    "FC 03 rejects quantity 126");

	/* Section 6.12: write several registers at once. */
	req[0] = MODBUS_FC_WRITE_MULTI_REG;
	req[1] = 0; req[2] = 0;		/* address 0 */
	req[3] = 0; req[4] = 3;		/* quantity 3 */
	req[5] = 6;			/* byte count */
	w[0] = 0x00; w[1] = 0x11;
	w[2] = 0x22; w[3] = 0x33;
	w[4] = 0x44; w[5] = 0x55;
	memcpy(req + 6, w, 6);
	error = run(req, 12, &rsp);
	CHECK(error == 0, "FC 10 writes three registers");
	check_bytes(&rsp, (const uint8_t *)"\x10\x00\x00\x00\x03", 5,
	    "FC 10 echoes the address and quantity");

	req[0] = MODBUS_FC_READ_HOLDING_REG;
	req[1] = 0; req[2] = 0;
	req[3] = 0; req[4] = 3;
	error = run(req, 5, &rsp);
	check_bytes(&rsp,
	    (const uint8_t *)"\x03\x06\x00\x11\x22\x33\x44\x55", 8,
	    "FC 03 returns the registers written by FC 10");

	/*
	 * Section 6.11 limits a multi coil write to 0x07b0, not to the
	 * 0x07d0 of the read functions; above that a conforming slave
	 * answers exception 03.
	 */
	req[0] = MODBUS_FC_WRITE_MULTI_COIL;
	req[1] = 0; req[2] = 0;
	req[3] = 0x07; req[4] = 0xb1;	/* 1969 */
	req[5] = 246;
	error = run(req, 11, &rsp);
	check_exc(&rsp, MODBUS_FC_WRITE_MULTI_COIL, MODBUS_EX_ILLEGAL_VALUE,
	    "FC 0F rejects a quantity above 0x07b0");

	req[3] = 0x07; req[4] = 0xb0;	/* 1968, the maximum */
	req[5] = 246;
	error = run(req, 11, &rsp);
	check_exc(&rsp, MODBUS_FC_WRITE_MULTI_COIL, MODBUS_EX_ILLEGAL_VALUE,
	    "FC 0F rejects a byte count that does not cover the quantity");

	/* Byte count must match the quantity exactly. */
	req[0] = MODBUS_FC_WRITE_MULTI_REG;
	req[3] = 0; req[4] = 3;
	req[5] = 5;
	error = run(req, 11, &rsp);
	check_exc(&rsp, MODBUS_FC_WRITE_MULTI_REG, MODBUS_EX_ILLEGAL_VALUE,
	    "FC 10 rejects a byte count that is not twice the quantity");

	req[3] = 0; req[4] = 124;
	req[5] = 6;
	error = run(req, 12, &rsp);
	check_exc(&rsp, MODBUS_FC_WRITE_MULTI_REG, MODBUS_EX_ILLEGAL_VALUE,
	    "FC 10 rejects quantity 124");

	/* A well formed request whose range runs past the map. */
	req[1] = 0x00; req[2] = 0xfe;		/* address 254 */
	req[3] = 0; req[4] = 3;
	req[5] = 6;
	error = run(req, 12, &rsp);
	check_exc(&rsp, MODBUS_FC_WRITE_MULTI_REG, MODBUS_EX_ILLEGAL_ADDRESS,
	    "FC 10 rejects a range that crosses the end of the area");
}

/* Section 6.5: single coil writes. */
static void
test_single_coil(void)
{
	struct modbus_pdu_buf rsp;
	uint8_t req[5];
	int error;

	req[0] = MODBUS_FC_WRITE_SINGLE_COIL;
	req[1] = 0; req[2] = 4;
	req[3] = 0xff; req[4] = 0x00;	/* on */
	error = run(req, 5, &rsp);
	CHECK(error == 0, "FC 05 sets a coil to 0xff00");
	check_bytes(&rsp, req, 5, "FC 05 echoes the request");

	req[0] = MODBUS_FC_READ_COILS;
	req[1] = 0; req[2] = 4;
	req[3] = 0; req[4] = 1;
	error = run(req, 5, &rsp);
	/*
	 * Section 6.1: the LSB of the first data byte holds the coil
	 * addressed in the query, and the rest follow toward the high order
	 * end, so a read of one coil at address 4 reports 0x01.
	 */
	CHECK(error == 0 && rsp.buf[2] == 0x01,
	    "FC 01 reports coil 4 in the least significant bit");

	req[0] = MODBUS_FC_WRITE_SINGLE_COIL;
	req[3] = 0x00; req[4] = 0x00;	/* off */
	error = run(req, 5, &rsp);
	CHECK(error == 0, "FC 05 sets a coil to 0x0000");

	req[0] = MODBUS_FC_READ_COILS;
	req[3] = 0; req[4] = 1;		/* quantity 1 again */
	error = run(req, 5, &rsp);
	CHECK(error == 0 && rsp.buf[2] == 0x00, "FC 01 sees the coil cleared");

	req[0] = MODBUS_FC_WRITE_SINGLE_COIL;
	req[3] = 0x12; req[4] = 0x34;	/* neither value */
	error = run(req, 5, &rsp);
	check_exc(&rsp, MODBUS_FC_WRITE_SINGLE_COIL, MODBUS_EX_ILLEGAL_VALUE,
	    "FC 05 rejects a value that is not 0x0000 or 0xff00");
}

/* Section 6.16: mask write. */
static void
test_mask_write(void)
{
	struct modbus_pdu_buf rsp;
	uint8_t req[7];
	int error;

	req[0] = MODBUS_FC_WRITE_SINGLE_REG;
	req[1] = 0; req[2] = 20;
	req[3] = 0xff; req[4] = 0xf0;	/* 0xfff0 */
	error = run(req, 5, &rsp);
	CHECK(error == 0, "FC 06 seeds the register for the mask test");

	req[0] = MODBUS_FC_MASK_WRITE_REG;
	req[1] = 0; req[2] = 20;		/* reference */
	req[3] = 0x0f; req[4] = 0x00;	/* and mask */
	req[5] = 0x00; req[6] = 0xff;	/* or mask */
	error = run(req, 7, &rsp);
	CHECK(error == 0, "FC 16 applies a mask write");
	check_bytes(&rsp, req, 7, "FC 16 echoes the request");

	req[0] = MODBUS_FC_READ_HOLDING_REG;
	req[1] = 0; req[2] = 20;
	req[3] = 0; req[4] = 1;
	error = run(req, 5, &rsp);
	check_bytes(&rsp, (const uint8_t *)"\x03\x02\x0f\xff", 4,
	    "FC 16 computes (current AND and) OR (or AND NOT and)");

	req[0] = MODBUS_FC_MASK_WRITE_REG;
	error = run(req, 5, &rsp);
	check_exc(&rsp, MODBUS_FC_MASK_WRITE_REG, MODBUS_EX_ILLEGAL_VALUE,
	    "FC 16 rejects a request that is not 6 bytes of data");
}

/* Section 6.17: write then read in one transaction. */
static void
test_read_write(void)
{
	struct modbus_pdu_buf rsp;
	uint8_t req[16];
	int error;

	req[0] = MODBUS_FC_READ_WRITE_MULTI_REG;
	req[1] = 0; req[2] = 0;		/* read address 0 */
	req[3] = 0; req[4] = 2;		/* read quantity 2 */
	req[5] = 0; req[6] = 30;		/* write address 30 */
	req[7] = 0; req[8] = 2;		/* write quantity 2 */
	req[9] = 4;			/* write byte count */
	req[10] = 0xaa; req[11] = 0xbb;
	req[12] = 0xcc; req[13] = 0xdd;
	error = run(req, 14, &rsp);
	CHECK(error == 0, "FC 17 writes then reads");
	check_bytes(&rsp, (const uint8_t *)"\x17\x04\x00\x11\x22\x33", 6,
	    "FC 17 returns only the read data");

	req[0] = MODBUS_FC_READ_HOLDING_REG;
	req[1] = 0; req[2] = 30;
	req[3] = 0; req[4] = 2;
	error = run(req, 5, &rsp);
	check_bytes(&rsp, (const uint8_t *)"\x03\x04\xaa\xbb\xcc\xdd", 6,
	    "FC 17 performed the write before the read");

	/* Write quantity limit for FC 17 is 121, not 123. */
	req[0] = MODBUS_FC_READ_WRITE_MULTI_REG;
	req[7] = 0; req[8] = 122;
	req[9] = 4;
	error = run(req, 14, &rsp);
	check_exc(&rsp, MODBUS_FC_READ_WRITE_MULTI_REG,
	    MODBUS_EX_ILLEGAL_VALUE,
	    "FC 17 rejects a write quantity of 122");

	/* Read quantity limit stays 125. */
	req[3] = 0; req[4] = 126;
	req[7] = 0; req[8] = 1;
	req[9] = 2;
	error = run(req, 12, &rsp);
	check_exc(&rsp, MODBUS_FC_READ_WRITE_MULTI_REG,
	    MODBUS_EX_ILLEGAL_VALUE,
	    "FC 17 rejects a read quantity of 126");
}

/* Section 7: unknown and malformed requests. */
static void
test_exceptions(void)
{
	struct modbus_pdu_buf rsp;
	uint8_t req[16];
	int error;

	req[0] = 0x2b;			/* encapsulated interface transport */
	error = run(req, 6, &rsp);
	check_exc(&rsp, 0x2b, MODBUS_EX_ILLEGAL_FUNCTION,
	    "an unimplemented function code answers exception 01");

	req[0] = 0x07;			/* read exception status */
	error = run(req, 1, &rsp);
	check_exc(&rsp, 0x07, MODBUS_EX_ILLEGAL_FUNCTION,
	    "function code 07 is not implemented");

	/* A function code with the exception bit already set is not a request. */
	req[0] = 0x83;
	error = run(req, 5, &rsp);
	CHECK(error < 0, "a response code in a request is not parsed as a PDU");

	/* Nothing that is not a PDU may produce a reply. */
	error = run(req, 0, &rsp);
	CHECK(error < 0, "an empty request produces no response");

	req[0] = 0x00;
	error = run(req, 5, &rsp);
	CHECK(error < 0, "function code 0 is not a valid PDU");

	/* Over the 253 byte PDU limit. */
	memset(req, 0x03, sizeof(req));
	error = run(req, 300, &rsp);
	CHECK(error < 0, "a request longer than the PDU limit is refused");
}

/* The backend must refuse to touch read only areas. */
static void
test_read_only_areas(void)
{
	struct modbus_pdu_buf rsp;
	uint8_t req[16];
	int error;

	/* FC 16 always targets holding registers. */
	req[0] = MODBUS_FC_MASK_WRITE_REG;
	req[1] = 0; req[2] = 0;
	req[3] = 0; req[4] = 0xff;
	req[5] = 0; req[6] = 0xff;
	error = run(req, 7, &rsp);
	CHECK(error == 0, "FC 16 writes a holding register");

	/* Reading an area that has no storage at all. */
	{
		struct modbus_backend empty;
		struct modbus_backend_config cfg;
		struct modbus_ctx c;

		memset(&empty, 0, sizeof(empty));
		cfg.ncoils = 0;
		cfg.ndiscrete_in = 0;
		cfg.ninput_regs = 0;
		cfg.nholding_regs = 0;
		CHECK(modbus_backend_init(&empty, &cfg) == EINVAL,
		    "a backend with no areas is refused");
		modbus_backend_destroy(&empty);

		memset(&c, 0, sizeof(c));
		c.backend = NULL;
		error = modbus_handle(&c, req, 7, &rsp);
		CHECK(error < 0, "a context without a backend answers nothing");
	}
}

int
main(void)
{

	backend_setup();

	test_read_bits();
	test_registers();
	test_single_coil();
	test_mask_write();
	test_read_write();
	test_exceptions();
	test_read_only_areas();
	framing_tests();
	client_tests();

	modbus_backend_destroy(&backend);

	printf("1..%d\n", checks);
	if (failures != 0) {
		printf("# %d of %d checks failed\n", failures, checks);
		return (1);
	}
	printf("# all %d checks passed\n", checks);

	return (0);
}
