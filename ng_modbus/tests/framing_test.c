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
 * Framing tests: CRC, LRC, and the RTU, ASCII and Modbus/TCP state machines.
 *
 * These feed the parsers one byte at a time with an injected timestamp, so
 * they cover stream fragmentation, coalescing, inter-character timing and
 * malformed input without a serial line, without a TCP stack and without
 * waiting for real time.
 */

#include <stdio.h>
#include <string.h>

#include <net/modbus/modbus.h>
#include <net/modbus/modbus_ascii.h>
#include <net/modbus/modbus_crc.h>
#include <net/modbus/modbus_rtu.h>
#include <net/modbus/modbus_tcp.h>

#include "test_util.h"

/*
 * Section 2.5.1.2 and appendix B of the Serial Line guide: the worked
 * example computes 0x1241 for the frame 02 07, and the low order byte is sent
 * first.
 */
static void
test_crc(void)
{
	uint8_t frame[2];
	uint16_t crc;

	frame[0] = 0x02;
	frame[1] = 0x07;
	crc = modbus_crc16(frame, sizeof(frame));
	CHECK(crc == 0x1241, "CRC of the appendix B example is 0x1241");

	CHECK(modbus_crc16(NULL, 0) == MODBUS_CRC_INIT,
	    "CRC of no bytes is the initial value");

	/* The same value computed in two steps must agree. */
	{
		uint16_t running = MODBUS_CRC_INIT;

		modbus_crc16_update(&running, frame, 1);
		modbus_crc16_update(&running, frame + 1, 1);
		CHECK(running == crc, "incremental CRC matches the one shot CRC");
	}
}

static void
test_lrc(void)
{
	/* Section 2.5.2.2: sum the bytes, then take the two's complement. */
	CHECK(modbus_lrc((const uint8_t *)"\x01\x04\x02\xff\xff", 5) == 0xfb,
	    "LRC of a read holding registers request");

	CHECK(modbus_lrc((const uint8_t *)"", 0) == 0x00,
	    "LRC of no bytes is zero");

	CHECK(modbus_lrc((const uint8_t *)"\xff", 1) == 0x01,
	    "LRC discards carries");
}

/* Character times from section 2.5.1.1. */
static void
test_rtu_timing(void)
{
	/*
	 * 9600 Bd with 11 bits per character is 1145833 ns per character, so
	 * t3.5 is 4010415 ns and t1.5 is 1718749 ns.
	 */
	CHECK(modbus_rtu_t35_ns(9600, 11) == 4010415,
	    "t3.5 at 9600 Bd is 3.5 character times");
	CHECK(modbus_rtu_t15_ns(9600, 11) == 1718749,
	    "t1.5 at 9600 Bd is 1.5 character times");

	/* Above 19200 Bd the specification fixes the timers. */
	CHECK(modbus_rtu_t35_ns(115200, 11) == 1750000,
	    "t3.5 above 19200 Bd is 1.750 ms");
	CHECK(modbus_rtu_t15_ns(115200, 11) == 750000,
	    "t1.5 above 19200 Bd is 750 us");

	CHECK(modbus_rtu_t35_ns(19200, 11) == 2005206,
	    "19200 Bd is still computed, not fixed");
	CHECK(modbus_rtu_t35_ns(0, 11) == 0, "a zero baud rate has no timing");
}

/* RTU framing, section 2.5.1.1. */
static void
test_rtu(void)
{
	struct modbus_rtu rtu;
	const uint8_t *frame;
	uint16_t len;
	uint16_t crc;
	uint8_t buf[8];
	uint64_t now;
	uint32_t t15;
	int i;

	/* Request: unit 1, read holding registers 0..9. */
	buf[0] = 0x01;
	buf[1] = 0x03;
	buf[2] = 0x00;
	buf[3] = 0x00;
	buf[4] = 0x00;
	buf[5] = 0x0a;
	crc = modbus_crc16(buf, 6);
	buf[6] = (uint8_t)(crc & 0xff);	/* low order byte first */
	buf[7] = (uint8_t)(crc >> 8);

	t15 = modbus_rtu_t15_ns(9600, 11);

	/* Bytes arriving one at a time, well inside t1.5, frame at the end. */
	modbus_rtu_init(&rtu);
	now = 0;
	for (i = 0; i < 8; i++) {
		enum modbus_rtu_result r;

		now += 100000;		/* 100 us per byte */
		r = modbus_rtu_input(&rtu, buf[i], now, t15);
		if (i < 7)
			CHECK(r == MODBUS_RTU_ACCEPTED,
			    "RTU accepts bytes before the CRC");
		else
			CHECK(r == MODBUS_RTU_FRAME,
			    "RTU completes a frame when the CRC lands");
	}
	frame = modbus_rtu_frame(&rtu, &len);
	CHECK(frame != NULL && len == 8, "RTU frame is address through CRC");
	if (frame != NULL)
		CHECK(frame[0] == 0x01 && frame[1] == 0x03,
		    "RTU frame starts with the address and function code");

	/* A corrupted CRC is never handed up. */
	modbus_rtu_init(&rtu);
	now = 0;
	for (i = 0; i < 8; i++) {
		uint8_t b = buf[i];

		if (i == 6)
			b ^= 0xff;
		now += 100000;
		modbus_rtu_input(&rtu, b, now, t15);
	}
	CHECK(modbus_rtu_frame(&rtu, &len) == NULL,
	    "RTU refuses a frame with a bad CRC");

	/* A silence longer than t1.5 inside a frame discards it. */
	modbus_rtu_init(&rtu);
	now = 0;
	for (i = 0; i < 4; i++) {
		now += 100000;
		modbus_rtu_input(&rtu, buf[i], now, t15);
	}
	now += (uint64_t)t15 * 2;
	CHECK(modbus_rtu_gap(&rtu, t15) == MODBUS_RTU_DISCARDED,
	    "RTU discards a frame abandoned by a long silence");
	CHECK(modbus_rtu_frame(&rtu, &len) == NULL,
	    "RTU has no frame after an incomplete one was dropped");

	/* A byte after the silence starts a fresh frame. */
	{
		enum modbus_rtu_result r;

		now += 100000;
		r = modbus_rtu_input(&rtu, buf[0], now, t15);
		CHECK(r == MODBUS_RTU_ACCEPTED,
		    "RTU starts a new frame after the silence");
	}

	/* An oversized run is discarded rather than overrunning the buffer. */
	modbus_rtu_init(&rtu);
	now = 0;
	for (i = 0; i < MODBUS_RTU_ADU_MAXLEN + 8; i++) {
		now += 1000;
		modbus_rtu_input(&rtu, 0x5a, now, t15);
	}
	CHECK(modbus_rtu_frame(&rtu, &len) == NULL,
	    "RTU refuses a frame longer than the maximum");
}

/* ASCII framing, section 2.5.2. */
static void
test_ascii(void)
{
	struct modbus_ascii ascii;
	const uint8_t *frame;
	uint8_t wire[64];
	uint8_t pdu[6];
	uint16_t len;
	size_t n, i;
	int frames;
	enum modbus_ascii_result r;

	pdu[0] = 0x01;		/* address */
	pdu[1] = 0x03;		/* read holding registers */
	pdu[2] = 0x00;
	pdu[3] = 0x00;
	pdu[4] = 0x00;
	pdu[5] = 0x0a;

	n = modbus_ascii_encode(pdu, sizeof(pdu), wire, sizeof(wire));
	CHECK(n == 1 + 2 * 7 + 2, "ASCII frame length is colon, body, CR LF");
	CHECK(wire[0] == ':' && wire[n - 2] == '\r' && wire[n - 1] == '\n',
	    "ASCII frame starts with a colon and ends with CR LF");
	CHECK(wire[1] == '0' && wire[2] == '1',
	    "ASCII encodes the address high nibble first");

	modbus_ascii_init(&ascii);
	frames = 0;
	for (i = 0; i < n; i++) {
		r = modbus_ascii_input(&ascii, wire[i]);
		if (r == MODBUS_ASCII_FRAME)
			frames++;
		if (r == MODBUS_ASCII_ERROR)
			break;
	}
	CHECK(frames == 1, "ASCII decoder returns one frame");
	frame = modbus_ascii_frame(&ascii, &len);
	CHECK(frame != NULL && len == 7,
	    "ASCII frame is address, PDU and LRC");
	if (frame != NULL) {
		CHECK(frame[0] == 0x01 && frame[1] == 0x03,
		    "ASCII decoded the address and function code");
		CHECK(frame[6] == modbus_lrc(frame, 6),
		    "ASCII frame carries a valid LRC");
	}

	/* A bad LRC is refused. */
	modbus_ascii_init(&ascii);
	frames = 0;
	for (i = 0; i < n; i++) {
		uint8_t c = wire[i];

		if (i == n - 3)		/* first LRC character */
			c = (c == '0') ? '1' : '0';
		r = modbus_ascii_input(&ascii, c);
		if (r == MODBUS_ASCII_FRAME)
			frames++;
	}
	CHECK(frames == 0, "ASCII refuses a frame with a bad LRC");

	/* A non hexadecimal character inside a frame is refused. */
	modbus_ascii_init(&ascii);
	r = modbus_ascii_input(&ascii, ':');
	CHECK(r == MODBUS_ASCII_ACCEPTED, "ASCII accepts the start colon");
	r = modbus_ascii_input(&ascii, 'Z');
	CHECK(r == MODBUS_ASCII_ERROR, "ASCII refuses a non hexadecimal digit");

	/*
	 * A frame that was read but not consumed must not be extended by
	 * the characters that follow it; only a colon starts a new one.
	 */
	modbus_ascii_init(&ascii);
	frames = 0;
	for (i = 0; i < n; i++) {
		r = modbus_ascii_input(&ascii, wire[i]);
		if (r == MODBUS_ASCII_FRAME)
			frames++;
	}
	CHECK(frames == 1, "ASCII produces the frame once");
	frame = modbus_ascii_frame(&ascii, &len);
	CHECK(frame != NULL && len == 7, "the frame stays readable");
	/* Bytes that arrive after a verified frame must not extend it. */
	modbus_ascii_input(&ascii, '0');
	modbus_ascii_input(&ascii, '1');
	CHECK(modbus_ascii_frame(&ascii, &len) == NULL,
	    "characters after a frame do not extend it");

	/* A second colon abandons the frame in progress. */
	modbus_ascii_init(&ascii);
	modbus_ascii_input(&ascii, ':');
	modbus_ascii_input(&ascii, '0');
	modbus_ascii_input(&ascii, '1');
	r = modbus_ascii_input(&ascii, ':');
	CHECK(r == MODBUS_ASCII_ACCEPTED,
	    "a colon restarts the ASCII frame");
	frame = modbus_ascii_frame(&ascii, &len);
	CHECK(frame == NULL, "the abandoned ASCII frame is gone");

	/* An odd number of digits is malformed. */
	modbus_ascii_init(&ascii);
	modbus_ascii_input(&ascii, ':');
	modbus_ascii_input(&ascii, '0');
	modbus_ascii_input(&ascii, '1');
	modbus_ascii_input(&ascii, '0');
	r = modbus_ascii_input(&ascii, '\r');
	CHECK(r == MODBUS_ASCII_ERROR, "ASCII refuses an odd digit count");
}

/* Modbus/TCP framing, section 3.1 of the TCP/IP guide. */
static void
test_tcp(void)
{
	struct modbus_tcp tcp;
	const uint8_t *pdu;
	uint8_t adu[16];
	uint8_t two[2 * 12];
	uint16_t len, tid;
	uint8_t unit;
	size_t n, i;
	int frames;

	pdu = (const uint8_t *)"\x03\x00\x10\x00\x02";
	n = modbus_tcp_encode(adu, sizeof(adu), 0x1234, 0x11,
	    (const uint8_t *)"\x03\x00\x10\x00\x02", 5);
	CHECK(n == 12, "MBAP plus a five byte PDU is twelve bytes");
	CHECK(adu[2] == 0 && adu[3] == 0, "the protocol identifier is zero");
	CHECK(adu[4] == 0 && adu[5] == 6,
	    "the length field counts the unit identifier and the PDU");

	/* One byte at a time: a stream knows nothing about packet boundaries. */
	modbus_tcp_init(&tcp);
	frames = 0;
	for (i = 0; i < n; i++) {
		if (modbus_tcp_input(&tcp, adu[i]) == MODBUS_TCP_FRAME)
			frames++;
	}
	CHECK(frames == 1, "TCP parser reassembles a byte at a time frame");
	pdu = modbus_tcp_frame(&tcp, &len, &tid, &unit);
	CHECK(len == 5 && tid == 0x1234 && unit == 0x11,
	    "TCP frame reports the transaction and unit identifiers");
	if (pdu != NULL)
		CHECK(pdu[0] == 0x03, "TCP frame yields the request PDU");

	/* Two requests arriving in one read are two frames. */
	memcpy(two, adu, n);
	modbus_tcp_encode(two + n, sizeof(two) - n, 0x1235, 0x11,
	    (const uint8_t *)"\x03\x00\x20\x00\x02", 5);
	modbus_tcp_init(&tcp);
	frames = 0;
	for (i = 0; i < n * 2; i++) {
		if (modbus_tcp_input(&tcp, two[i]) == MODBUS_TCP_FRAME) {
			frames++;
			modbus_tcp_release(&tcp);
		}
	}
	CHECK(frames == 2, "TCP parser splits two coalesced requests");

	/* A bad protocol identifier invalidates the stream. */
	modbus_tcp_init(&tcp);
	adu[2] = 1;
	CHECK(modbus_tcp_input(&tcp, adu[0]) == MODBUS_TCP_ACCEPTED,
	    "TCP accepts header bytes while they arrive");
	modbus_tcp_init(&tcp);
	i = 0;
	while (i < MODBUS_TCP_MBAP_FIXED) {
		if (modbus_tcp_input(&tcp, adu[i++]) == MODBUS_TCP_ERROR)
			break;
	}
	CHECK(i == MODBUS_TCP_MBAP_FIXED,
	    "TCP rejects a non zero protocol identifier");

	/* A length field larger than the maximum PDU is refused. */
	modbus_tcp_init(&tcp);
	adu[2] = 0;
	adu[4] = 0xff;
	adu[5] = 0xff;
	for (i = 0; i < 6; i++) {
		if (modbus_tcp_input(&tcp, adu[i]) == MODBUS_TCP_ERROR)
			break;
	}
	/* The length field is the sixth byte, so that is where it is caught. */
	CHECK(i == 5, "TCP rejects an oversized length field before buffering");

	/* A length field of one leaves no room for a PDU. */
	modbus_tcp_init(&tcp);
	adu[4] = 0;
	adu[5] = 1;
	frames = 0;
	for (i = 0; i < 6; i++) {
		if (modbus_tcp_input(&tcp, adu[i]) == MODBUS_TCP_ERROR)
			frames++;
	}
	CHECK(frames == 1, "TCP rejects a length field of one");

	/* Encoding refuses a PDU that does not fit. */
	CHECK(modbus_tcp_encode(adu, sizeof(adu), 1, 1, pdu, 0) == 0,
	    "TCP encoder refuses an empty PDU");
	CHECK(modbus_tcp_encode(adu, 4, 1, 1, pdu, 5) == 0,
	    "TCP encoder refuses a short output buffer");
}

void
framing_tests(void)
{

	test_crc();
	test_lrc();
	test_rtu_timing();
	test_rtu();
	test_ascii();
	test_tcp();
}