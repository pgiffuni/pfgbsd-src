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

#ifndef _NETGRAPH_NG_POWERLINK_H_
#define	_NETGRAPH_NG_POWERLINK_H_

#include <sys/param.h>
#include <sys/types.h>

#include <net/powerlink/powerlink.h>

/*
 * This header is included by userland clients of the node, so it must not
 * pull in the kernel netgraph.h.  Nothing here expands the parse tables
 * below, so a client that only reads a structure is unaffected; ngctl reaches
 * the field descriptions through the module.
 */

#define	NG_POWERLINK_COOKIE	1790918000UL	/* creation date */

#define	NG_POWERLINK_HOOK_LOWER	"lower"

/* Statistics reported by NGM_POWERLINK_GET_STATS. */
struct ng_powerlink_stats {
	uint64_t	rx_frames;
	uint64_t	rx_bytes;
	uint64_t	tx_frames;
	uint64_t	tx_bytes;
	uint64_t	rx_dropped;
	uint64_t	errors;
	uint64_t	cycle_count;
	uint64_t	deadline_misses;
	uint64_t	wakeup_latency_max_ns;
	uint64_t	cycle_duration_max_ns;
};

#define	NG_POWERLINK_STATS_INFO	{			\
	{ "rx_frames",		&ng_parse_uint64_type	},	\
	{ "rx_bytes",		&ng_parse_uint64_type	},	\
	{ "tx_frames",		&ng_parse_uint64_type	},	\
	{ "tx_bytes",		&ng_parse_uint64_type	},	\
	{ "rx_dropped",		&ng_parse_uint64_type	},	\
	{ "errors",		&ng_parse_uint64_type	},	\
	{ "cycle_count",	&ng_parse_uint64_type	},	\
	{ "deadline_misses",	&ng_parse_uint64_type	},	\
	{ "wakeup_latency_max_ns",	&ng_parse_uint64_type },	\
	{ "cycle_duration_max_ns",	&ng_parse_uint64_type },	\
	{ NULL }					\
}

/* Status reported by NGM_POWERLINK_GET_STATUS. */
struct ng_powerlink_status {
	uint32_t	abi_version;
	uint32_t	state;		/* enum powerlink_state */
	uint32_t	mode;		/* enum powerlink_mode */
	uint32_t	cycle_len_us;
	uint32_t	node_id;
	uint32_t	edrv_kind;	/* enum powerlink_edrv_kind */
	uint32_t	reserved;
};

#define	NG_POWERLINK_STATUS_INFO	{			\
	{ "abi_version",	&ng_parse_uint32_type	},	\
	{ "state",		&ng_parse_uint32_type	},	\
	{ "mode",		&ng_parse_uint32_type	},	\
	{ "cycle_len_us",	&ng_parse_uint32_type	},	\
	{ "node_id",		&ng_parse_uint32_type	},	\
	{ "edrv_kind",	&ng_parse_uint32_type	},	\
	{ NULL }					\
}

enum {
	NGM_POWERLINK_SET_CONFIG = 1,	/* struct powerlink_config */
	NGM_POWERLINK_GET_CONFIG,	/* returns struct powerlink_config */
	NGM_POWERLINK_START,
	NGM_POWERLINK_STOP,
	NGM_POWERLINK_GET_STATUS,	/* returns struct ng_powerlink_status */
	NGM_POWERLINK_GET_STATS,	/* returns struct ng_powerlink_stats */
	NGM_POWERLINK_RESET_STATS
};

#endif /* _NETGRAPH_NG_POWERLINK_H_ */
