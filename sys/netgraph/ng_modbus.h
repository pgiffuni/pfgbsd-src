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

#ifndef _NETGRAPH_NG_MODBUS_H_
#define	_NETGRAPH_NG_MODBUS_H_

#include <sys/param.h>
#include <sys/types.h>

#include <net/modbus/modbus.h>

/*
 * This header is included by userland clients, so it must not pull in the
 * kernel netgraph.h, which refuses to be used outside the kernel.  A userland
 * program includes <netgraph.h> from libnetgraph first, which is where the
 * parse types below come from; the kernel build gets the kernel half here.
 * This is the arrangement every other node header uses.
 */
#ifdef _KERNEL
#include <netgraph/netgraph.h>
#endif

/* Node type name, must fit NG_TYPESIZ. */
#define	NG_MODBUS_NODE_TYPE	"modbus"

/* Control message cookie, change when any message below changes. */
#define	NGM_MODBUS_COOKIE	1790909000UL	/* creation date */

/* Hook names. */
#define	NG_MODBUS_HOOK_LOWER	"lower"		/* transport data */

/* Framing used on the lower hook. */
#define	NG_MODBUS_TRANSPORT_NONE	0	/* none yet, raw byte stream */
#define	NG_MODBUS_TRANSPORT_VNET	1	/* netgraph virtual transport */
#define	NG_MODBUS_TRANSPORT_TCP		2	/* MBAP framed */
#define	NG_MODBUS_TRANSPORT_RTU		3	/* CRC framed */
#define	NG_MODBUS_TRANSPORT_ASCII	4	/* LRC framed */

/*
 * How long a client waits for a response before giving the transaction up.
 * Section 4.5 of the specification leaves the timing to the application, and
 * this module keeps one transaction at a time and never retries.
 */
#define	MODBUS_CLIENT_TIMEOUT_MS	1000

/* Serial line defaults; only used by the RTU and ASCII transports. */
#define	MODBUS_DEFAULT_BAUD		9600
#define	MODBUS_DEFAULT_BITS_PER_CHAR	11	/* 8E1 or 8N2 */

/* Version of the structures below.  Bumped on any wire change. */
#define	NG_MODBUS_ABI_VERSION	2

/*
 * Configuration.  Fixed width types only: this structure crosses the
 * netgraph control message boundary and is part of the module ABI.
 */
struct ng_modbus_config {
	uint8_t		role;		/* MODBUS_ROLE_* */
	uint8_t		transport;	/* NG_MODBUS_TRANSPORT_* */
	uint8_t		unit_id;	/* 0 broadcast, 1..247 unit */
	uint8_t		reserved;
};

/* Keep this in sync with the above structure definition. */
#define	NG_MODBUS_CONFIG_INFO	{			\
	{ "role",	&ng_parse_uint8_type	},	\
	{ "transport",	&ng_parse_uint8_type	},	\
	{ "unit_id",	&ng_parse_uint8_type	},	\
	{ "reserved",	&ng_parse_uint8_type	},	\
	{ NULL }					\
}

/* Read-only description of what this build implements. */
struct ng_modbus_info {
	uint32_t	abi_version;
	uint32_t	max_pdu;		/* MODBUS_PDU_MAXLEN */
	uint32_t	max_adu;		/* largest ADU, 0 if none */
	uint32_t	transports;	/* bit per supported framing */
	uint32_t	baud;		/* serial line rate */
	uint32_t	bits_per_char;	/* bits per character */
};

/* Keep this in sync with the above structure definition. */
#define	NG_MODBUS_INFO_INFO	{			\
	{ "abi_version",	&ng_parse_uint32_type	},	\
	{ "max_pdu",	&ng_parse_uint32_type	},	\
	{ "max_adu",	&ng_parse_uint32_type	},	\
	{ "transports",	&ng_parse_uint32_type	},	\
	{ "baud",	&ng_parse_uint32_type	},	\
	{ "bits_per_char",	&ng_parse_uint32_type	},	\
	{ NULL }					\
}

/*
 * A client request, carried in NGM_MODBUS_REQUEST.  The PDU is a fixed array
 * of bytes rather than a string, because a PDU may contain any byte value
 * including a zero and the interface is binary; the parse type below lets
 * ngctl pass it as a hexadecimal string.
 */
struct ng_modbus_request {
	uint8_t		pdu[MODBUS_PDU_MAXLEN];
	uint8_t		unit_id;	/* address to send to */
	uint16_t	pdulen;		/* bytes of pdu to send */
	uint16_t	tid;		/* transaction identifier, Modbus/TCP */
	uint16_t	reserved;
};

/* Keep this in sync with the above structure definition. */
#define	NG_MODBUS_REQUEST_INFO(pdutype)	{		\
	{ "pdu",		(pdutype)		},	\
	{ "unit_id",		&ng_parse_uint8_type	},	\
	{ "pdulen",		&ng_parse_uint16_type	},	\
	{ "tid",		&ng_parse_uint16_type	},	\
	{ "reserved",		&ng_parse_uint16_type	},	\
	{ NULL }					\
}

/* The response of a completed client transaction. */
struct ng_modbus_response {
	uint8_t		pdu[MODBUS_PDU_MAXLEN];
	uint8_t		state;		/* enum modbus_client_result */
	uint16_t	pdulen;
	uint16_t	reserved;
};

/* Keep this in sync with the above structure definition. */
#define	NG_MODBUS_RESPONSE_INFO(pdutype)	{		\
	{ "pdu",		(pdutype)		},	\
	{ "state",		&ng_parse_uint8_type	},	\
	{ "pdulen",		&ng_parse_uint16_type	},	\
	{ "reserved",		&ng_parse_uint16_type	},	\
	{ NULL }					\
}

/* Sizes of the four register areas; see modbus_backend.h. */
struct ng_modbus_backend {
	uint16_t	ncoils;
	uint16_t	ndiscrete_in;
	uint16_t	ninput_regs;
	uint16_t	nholding_regs;
};

/* Keep this in sync with the above structure definition. */
#define	NG_MODBUS_BACKEND_INFO	{			\
	{ "ncoils",	&ng_parse_uint16_type	},	\
	{ "ndiscrete_in",	&ng_parse_uint16_type	},	\
	{ "ninput_regs",	&ng_parse_uint16_type	},	\
	{ "nholding_regs",	&ng_parse_uint16_type	},	\
	{ NULL }					\
}

/* Counters, see ng_modbus(4). */
struct ng_modbus_stats {
	uint64_t	rx_frames;
	uint64_t	rx_bytes;
	uint64_t	tx_frames;
	uint64_t	tx_bytes;
	uint64_t	errors;
	uint64_t	timeouts;
};

/* Keep this in sync with the above structure definition. */
#define	NG_MODBUS_STATS_INFO	{			\
	{ "rx_frames",	&ng_parse_uint64_type	},	\
	{ "rx_bytes",	&ng_parse_uint64_type	},	\
	{ "tx_frames",	&ng_parse_uint64_type	},	\
	{ "tx_bytes",	&ng_parse_uint64_type	},	\
	{ "errors",	&ng_parse_uint64_type	},	\
	{ "timeouts",	&ng_parse_uint64_type	},	\
	{ NULL }					\
}

/*
 * The parse tables, not these structures, define the payload length a
 * userland client sends, and rcvmsg() rejects anything that does not match
 * sizeof().  Adding a member without adding a table entry would silently
 * make the text interface unusable, so fail the build instead.
 */
_Static_assert(sizeof(struct ng_modbus_config) == 4,
    "NG_MODBUS_CONFIG_INFO must list every member");
_Static_assert(sizeof(struct ng_modbus_info) == 24,
    "NG_MODBUS_INFO_INFO must list every member");
_Static_assert(sizeof(struct ng_modbus_stats) == 48,
    "NG_MODBUS_STATS_INFO must list every member");
_Static_assert(sizeof(struct ng_modbus_request) == 260,
    "NG_MODBUS_REQUEST_INFO must list every member");
_Static_assert(sizeof(struct ng_modbus_response) == 258,
    "NG_MODBUS_RESPONSE_INFO must list every member");
_Static_assert(sizeof(struct ng_modbus_backend) == 8,
    "NG_MODBUS_BACKEND_INFO must list every member");

/* Netgraph commands. */
enum {
	NGM_MODBUS_SET_CONFIG = 1,	/* supply a struct ng_modbus_config */
	NGM_MODBUS_GET_CONFIG,		/* returns a struct ng_modbus_config */
	NGM_MODBUS_GET_INFO,		/* returns a struct ng_modbus_info */
	NGM_MODBUS_REQUEST,		/* supply a struct ng_modbus_request */
	NGM_MODBUS_GET_RESPONSE,		/* returns the response */
	NGM_MODBUS_GET_BACKEND,		/* returns a struct ng_modbus_backend */
	NGM_MODBUS_SET_BACKEND,		/* supply a struct ng_modbus_backend */
	NGM_MODBUS_GET_STATS,		/* returns a struct ng_modbus_stats */
	NGM_MODBUS_CLR_STATS,		/* supply nothing */
};

#endif /* _NETGRAPH_NG_MODBUS_H_ */
