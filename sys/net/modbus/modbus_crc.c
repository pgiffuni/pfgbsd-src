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
 * MODBUS CRC-16.
 *
 * Section 2.5.1.2 of the MODBUS over Serial Line Specification V1.02: the
 * register is preloaded with all ones, each byte is exclusive ORed into it,
 * and the register is then shifted eight times toward the least significant
 * bit, each time exclusive ORing the fixed value 0xa001 when the bit shifted
 * out was one.  The value left in the register is the CRC.
 */

#include <sys/param.h>
#include <sys/types.h>

#include "modbus_crc.h"

void
modbus_crc16_update(uint16_t *crc, const uint8_t *buf, size_t len)
{
	uint16_t reg = *crc;
	size_t i;
	unsigned int bit;

	for (i = 0; i < len; i++) {
		reg ^= buf[i];
		for (bit = 0; bit < 8; bit++) {
			if (reg & 1)
				reg = (uint16_t)((reg >> 1) ^
				    MODBUS_CRC_POLY);
			else
				reg = (uint16_t)(reg >> 1);
		}
	}

	*crc = reg;
}

uint16_t
modbus_crc16(const uint8_t *buf, size_t len)
{
	uint16_t crc = MODBUS_CRC_INIT;

	modbus_crc16_update(&crc, buf, len);

	return (crc);
}
