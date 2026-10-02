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

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/callout.h>
#include <sys/time.h>
#include <sys/kernel.h>
#include <sys/malloc.h>
#include <sys/mbuf.h>
#include <sys/errno.h>

#include <netgraph/ng_message.h>
#include <netgraph/netgraph.h>
#include <netgraph/ng_parse.h>

#include <net/modbus/modbus.h>
#include <net/modbus/modbus_ascii.h>
#include <net/modbus/modbus_backend.h>
#include <net/modbus/modbus_client.h>
#include <net/modbus/modbus_crc.h>
#include <net/modbus/modbus_fc.h>
#include <net/modbus/modbus_pdu.h>
#include <net/modbus/modbus_rtu.h>
#include <net/modbus/modbus_tcp.h>

#include "ng_modbus.h"

static MALLOC_DEFINE(M_NETGRAPH_MODBUS, "netgraph_modbus",
    "netgraph modbus node");

/* Node private data. */
struct ng_modbus_private {
	struct ng_modbus_config	 cfg;
	struct ng_modbus_stats	 stats;
	struct modbus_backend	 backend;
	struct modbus_backend	 newbe;
	struct ng_modbus_backend be;
	struct modbus_ctx	 ctx;

	/* One parser per framing; only the configured one is driven. */
	struct modbus_tcp	 tcp;
	struct modbus_rtu	 rtu;
	struct modbus_ascii	 ascii;

	/* Client side transaction state. */
	struct modbus_client	 client;
	struct ng_modbus_request nq;
	struct ng_modbus_response rsp;

	/*
	 * Timers.  Each is created here and drained in shutdown.  The
	 * handler runs in callout context and therefore touches nothing but
	 * the node pointer: it re-enters the node with ng_send_fn(), which
	 * runs in the writer class with the node alive.
	 */
	struct callout		 client_timer;
	struct callout		 rtu_timer;

	/* Serial line parameters, only used by the RTU transport. */
	uint32_t		 baud;
	uint8_t			 bits_per_char;

	hook_p			 lower;
	node_p			 node;
};

typedef struct ng_modbus_private *ng_modbus_p;

/* Parse types for the control message structures. */
static const struct ng_parse_struct_field ng_modbus_config_type_fields[]
	= NG_MODBUS_CONFIG_INFO;
static const struct ng_parse_struct_field ng_modbus_info_type_fields[]
	= NG_MODBUS_INFO_INFO;
static const struct ng_parse_struct_field ng_modbus_stats_type_fields[]
	= NG_MODBUS_STATS_INFO;

static const struct ng_parse_type ng_modbus_config_type = {
	&ng_parse_struct_type,
	ng_modbus_config_type_fields
};

static const struct ng_parse_type ng_modbus_info_type = {
	&ng_parse_struct_type,
	ng_modbus_info_type_fields
};

/*
 * The PDU inside the request and response messages is a fixed array of
 * bytes, following the same idiom as ng_one2many(4).
 */
static const struct ng_parse_fixedarray_info ng_modbus_pdu_array_info = {
	&ng_parse_uint8_type,
	MODBUS_PDU_MAXLEN
};

static const struct ng_parse_type ng_modbus_pdu_array_type = {
	&ng_parse_fixedarray_type,
	&ng_modbus_pdu_array_info,
};

static const struct ng_parse_type ng_modbus_stats_type = {
	&ng_parse_struct_type,
	ng_modbus_stats_type_fields
};

static const struct ng_parse_struct_field ng_modbus_request_type_fields[]
	= NG_MODBUS_REQUEST_INFO(&ng_modbus_pdu_array_type);

static const struct ng_parse_type ng_modbus_request_type = {
	&ng_parse_struct_type,
	ng_modbus_request_type_fields
};

static const struct ng_parse_struct_field ng_modbus_response_type_fields[]
	= NG_MODBUS_RESPONSE_INFO(&ng_modbus_pdu_array_type);

static const struct ng_parse_type ng_modbus_response_type = {
	&ng_parse_struct_type,
	ng_modbus_response_type_fields
};

static const struct ng_parse_struct_field ng_modbus_backend_type_fields[]
	= NG_MODBUS_BACKEND_INFO;

static const struct ng_parse_type ng_modbus_backend_type = {
	&ng_parse_struct_type,
	ng_modbus_backend_type_fields
};

/* List of commands and how to convert arguments to/from ASCII. */
static const struct ng_cmdlist ng_modbus_cmds[] = {
	{ NGM_MODBUS_COOKIE,
	  NGM_MODBUS_SET_CONFIG,
	  "setconfig",
	  &ng_modbus_config_type,
	  &ng_modbus_config_type
	},
	{
	  NGM_MODBUS_COOKIE,
	  NGM_MODBUS_GET_CONFIG,
	  "getconfig",
	  NULL,
	  &ng_modbus_config_type
	},
	{
	  NGM_MODBUS_COOKIE,
	  NGM_MODBUS_GET_INFO,
	  "getinfo",
	  NULL,
	  &ng_modbus_info_type
	},
	{
	  NGM_MODBUS_COOKIE,
	  NGM_MODBUS_REQUEST,
	  "request",
	  &ng_modbus_request_type,
	  NULL
	},
	{
	  NGM_MODBUS_COOKIE,
	  NGM_MODBUS_GET_RESPONSE,
	  "getresponse",
	  NULL,
	  &ng_modbus_response_type
	},
	{
	  NGM_MODBUS_COOKIE,
	  NGM_MODBUS_GET_BACKEND,
	  "getbackend",
	  NULL,
	  &ng_modbus_backend_type
	},
	{
	  NGM_MODBUS_COOKIE,
	  NGM_MODBUS_SET_BACKEND,
	  "setbackend",
	  &ng_modbus_backend_type,
	  &ng_modbus_backend_type
	},
	{
	  NGM_MODBUS_COOKIE,
	  NGM_MODBUS_GET_STATS,
	  "getstats",
	  NULL,
	  &ng_modbus_stats_type
	},
	{
	  NGM_MODBUS_COOKIE,
	  NGM_MODBUS_CLR_STATS,
	  "clrstats",
	  NULL,
	  NULL
	},
	{ 0 }
};

static int ng_modbus_constructor(node_p node);
static int ng_modbus_rcvmsg(node_p node, item_p item, hook_p lasthook);
static int ng_modbus_rcvdata(hook_p hook, item_p item);
static int ng_modbus_newhook(node_p node, hook_p hook, const char *name);
static int ng_modbus_disconnect(hook_p hook);
static int ng_modbus_shutdown(node_p node);
static void ng_modbus_send_response(ng_modbus_p priv, hook_p hook,
    const uint8_t *pdu, uint16_t pdulen, uint16_t tid, uint8_t unit);
static void ng_modbus_client_expired_fn(node_p node, hook_p hook, void *arg1,
    int arg2);
static void ng_modbus_rtu_gap_fn(node_p node, hook_p hook, void *arg1,
    int arg2);

/*
 * Both timers run their handler in callout context, where nothing may touch
 * node state.  The handler only re-enters the node through ng_send_fn(), which
 * queues a function item: that runs in the writer class, with the node
 * reference held by the item, so the node cannot be freed underneath it.
 */
static void
ng_modbus_client_timeout(void *arg)
{
	node_p node = arg;

	ng_send_fn(node, NULL, ng_modbus_client_expired_fn, NULL, 0);
}

static void
ng_modbus_rtu_gap_callout(void *arg)
{
	node_p node = arg;

	ng_send_fn(node, NULL, ng_modbus_rtu_gap_fn, NULL, 0);
}

static void
ng_modbus_client_expired_fn(node_p node, hook_p hook, void *arg1, int arg2)
{
	ng_modbus_p priv = NG_NODE_PRIVATE(node);

	(void)hook;
	(void)arg1;
	(void)arg2;

	if (priv == NULL)
		return;

	if (modbus_client_expired(&priv->client) == MODBUS_CLIENT_EXPIRED)
		priv->stats.timeouts++;
}

/*
 * A frame that was still being collected when the line went quiet is
 * incomplete and has to be dropped, which is section 2.5.1.1 of the Serial
 * Line guide.
 */
static void
ng_modbus_rtu_gap_fn(node_p node, hook_p hook, void *arg1, int arg2)
{
	ng_modbus_p priv = NG_NODE_PRIVATE(node);
	uint32_t t15_ns;

	(void)hook;
	(void)arg1;
	(void)arg2;

	if (priv == NULL)
		return;

	t15_ns = modbus_rtu_t15_ns(priv->baud, priv->bits_per_char);
	if (modbus_rtu_gap(&priv->rtu, t15_ns) == MODBUS_RTU_DISCARDED)
		priv->stats.errors++;
}

static struct ng_type ng_modbus_typestruct = {
	.version =	NG_ABI_VERSION,
	.name =		NG_MODBUS_NODE_TYPE,
	.constructor =	ng_modbus_constructor,
	.rcvmsg =	ng_modbus_rcvmsg,
	.shutdown =	ng_modbus_shutdown,
	.newhook =	ng_modbus_newhook,
	.rcvdata =	ng_modbus_rcvdata,
	.disconnect =	ng_modbus_disconnect,
	.cmdlist =	ng_modbus_cmds,
};
/*
 * NETGRAPH_INIT() names the module after its type, so it would register as
 * "ng_modbus" while the KLD is modbus.ko and is loaded as "modbus".  Declare
 * the module under the file's name instead so kldstat, modstat and module
 * dependencies all agree.  The price is that "ngctl" can no longer autoload
 * this node by type name (ng_socket.c:251-282 looks for ng_modbus); it must
 * be kldloaded first.
 */
static moduledata_t modbus_moddata = {
	"modbus",
	ng_mod_event,
	&ng_modbus_typestruct
};

DECLARE_MODULE(modbus, modbus_moddata, SI_SUB_PSEUDO, SI_ORDER_MIDDLE);
MODULE_DEPEND(modbus, netgraph, NG_ABI_VERSION, NG_ABI_VERSION,
    NG_ABI_VERSION);

/*
 * Validate a configuration supplied by a control message.  Rejecting bad
 * values here keeps the protocol layer free of range checks and keeps a
 * misconfigured node from generating frames no peer could parse.
 */
static int
ng_modbus_check_config(const struct ng_modbus_config *cfg)
{

	switch (cfg->role) {
	case MODBUS_ROLE_SERVER:
	case MODBUS_ROLE_CLIENT:
		break;
	default:
		return (EINVAL);
	}

	switch (cfg->transport) {
	case NG_MODBUS_TRANSPORT_NONE:
	case NG_MODBUS_TRANSPORT_VNET:
	case NG_MODBUS_TRANSPORT_TCP:
	case NG_MODBUS_TRANSPORT_RTU:
	case NG_MODBUS_TRANSPORT_ASCII:
		break;
	default:
		return (EINVAL);
	}

	/*
	 * 0 is the broadcast address; 1..247 are unit addresses; 248..255
	 * are reserved by the Serial Line guide section 2.2 and can never be
	 * matched by a peer.
	 */
	if (cfg->unit_id > MODBUS_UNIT_MAX)
		return (EINVAL);

	return (0);
}

static int
ng_modbus_constructor(node_p node)
{
	struct modbus_backend_config backend;
	ng_modbus_p priv;
	int error;

	priv = malloc(sizeof(*priv), M_NETGRAPH_MODBUS, M_WAITOK | M_ZERO);
	if (priv == NULL)
		return (ENOMEM);

	priv->cfg.role = MODBUS_ROLE_SERVER;
	priv->cfg.transport = NG_MODBUS_TRANSPORT_NONE;
	priv->cfg.unit_id = 1;
	priv->lower = NULL;
	priv->node = node;

	backend.ncoils = MODBUS_BACKEND_DEFAULT;
	backend.ndiscrete_in = MODBUS_BACKEND_DEFAULT;
	backend.ninput_regs = MODBUS_BACKEND_DEFAULT;
	backend.nholding_regs = MODBUS_BACKEND_DEFAULT;

	if ((error = modbus_backend_init(&priv->backend, &backend)) != 0) {
		free(priv, M_NETGRAPH_MODBUS);
		return (error);
	}

	priv->ctx.backend = &priv->backend;
	priv->ctx.role = MODBUS_ROLE_SERVER;
	priv->ctx.unit_id = priv->cfg.unit_id;

	priv->baud = MODBUS_DEFAULT_BAUD;
	priv->bits_per_char = MODBUS_DEFAULT_BITS_PER_CHAR;
	modbus_tcp_init(&priv->tcp);
	modbus_rtu_init(&priv->rtu);
	modbus_ascii_init(&priv->ascii);
	modbus_client_reset(&priv->client);
	callout_init(&priv->client_timer, 0);
	callout_init(&priv->rtu_timer, 0);

	NG_NODE_SET_PRIVATE(node, priv);

	/*
	 * The framework serializes items per node under a reader/writer
	 * gate, and data items run in the reader class.  Asking for writer
	 * access makes every callback mutually exclusive, which lets this
	 * node run without a lock of its own.
	 */
	NG_NODE_FORCE_WRITER(node);

	return (0);
}

static int
ng_modbus_newhook(node_p node, hook_p hook, const char *name)
{
	ng_modbus_p priv;

	if (strcmp(name, NG_MODBUS_HOOK_LOWER) != 0)
		return (EINVAL);

	priv = NG_NODE_PRIVATE(node);
	if (priv == NULL)
		return (EINVAL);

	if (priv->lower != NULL)
		return (EISCONN);

	priv->lower = hook;
	NG_HOOK_SET_PRIVATE(hook, priv);

	return (0);
}

static int
ng_modbus_disconnect(hook_p hook)
{
	ng_modbus_p priv;
	node_p node;

	node = NG_HOOK_NODE(hook);
	priv = NG_HOOK_PRIVATE(hook);
	if (priv == NULL)
		return (EINVAL);

	if (priv->lower == hook)
		priv->lower = NULL;
	NG_HOOK_SET_PRIVATE(hook, NULL);

	if (NG_NODE_NUMHOOKS(node) == 0 && NG_NODE_IS_VALID(node))
		ng_rmnode_self(node);

	return (0);
}

/*
 * Control messages.  Only fixed size structures cross this boundary, and the
 * version reported by NGM_MODBUS_GET_INFO tells a userland client which
 * layout it is talking to.
 */
static int
ng_modbus_rcvmsg(node_p node, item_p item, hook_p lasthook)
{
	ng_modbus_p priv;
	struct ng_mesg *msg;
	struct ng_mesg *resp;
	int error;

	NGI_GET_MSG(item, msg);
	if (msg == NULL)
		return (ENOMEM);

	priv = NG_NODE_PRIVATE(node);
	resp = NULL;
	error = 0;
	if (priv == NULL) {
		error = EINVAL;
		goto done;
	}

	switch (msg->header.typecookie) {
	case NGM_MODBUS_COOKIE:
		switch (msg->header.cmd) {
		case NGM_MODBUS_SET_CONFIG:
			if (msg->header.arglen != sizeof(priv->cfg)) {
				error = EINVAL;
				break;
			}
			error = ng_modbus_check_config(
			    (const struct ng_modbus_config *)msg->data);
			if (error != 0)
				break;
			bcopy(msg->data, &priv->cfg, sizeof(priv->cfg));
			priv->cfg.reserved = 0;
			priv->ctx.role = priv->cfg.role;
			priv->ctx.unit_id = priv->cfg.unit_id;
			/*
			 * A transport change invalidates any partial frame
			 * the old one had collected.
			 */
			modbus_tcp_init(&priv->tcp);
			modbus_rtu_init(&priv->rtu);
			modbus_ascii_init(&priv->ascii);
			modbus_client_reset(&priv->client);
			callout_stop(&priv->client_timer);
			callout_stop(&priv->rtu_timer);
			break;

		case NGM_MODBUS_GET_CONFIG:
			NG_MKRESPONSE(resp, msg, sizeof(priv->cfg), M_NOWAIT);
			if (resp == NULL) {
				error = ENOMEM;
				break;
			}
			bcopy(&priv->cfg, resp->data, sizeof(priv->cfg));
			break;

		case NGM_MODBUS_GET_INFO: {
			struct ng_modbus_info *info;

			NG_MKRESPONSE(resp, msg, sizeof(*info), M_NOWAIT);
			if (resp == NULL) {
				error = ENOMEM;
				break;
			}
			info = (struct ng_modbus_info *)resp->data;
			info->abi_version = NG_MODBUS_ABI_VERSION;
			info->max_pdu = MODBUS_PDU_MAXLEN;
			/*
			 * No framer is implemented yet; a client must not be
			 * told this node speaks TCP, RTU or ASCII.  Each
			 * framer sets its own bit here when it lands.
			 */
			info->max_adu = 0;
			info->transports = 0;
			info->baud = priv->baud;
			info->bits_per_char = priv->bits_per_char;
			break;
		}

		case NGM_MODBUS_REQUEST: {
			const struct ng_modbus_request *nq;
			const uint8_t *pdu;
			uint16_t len, unit, tid;

			if (msg->header.arglen != sizeof(priv->nq)) {
				error = EINVAL;
				break;
			}
			nq = (const struct ng_modbus_request *)msg->data;
			if (nq->pdulen == 0 || nq->pdulen > MODBUS_PDU_MAXLEN ||
			    nq->unit_id > MODBUS_UNIT_MAX) {
				error = EINVAL;
				break;
			}
			if (priv->lower == NULL) {
				/*
				 * Nowhere to send it, so do not start a
				 * transaction that cannot be finished.
				 */
				error = ENXIO;
				break;
			}

			error = modbus_client_start(&priv->client, nq->pdu,
			    nq->pdulen, nq->unit_id, nq->tid);
			if (error != 0)
				break;

			pdu = modbus_client_request(&priv->client, &len,
			    &unit, &tid);
			if (pdu != NULL) {
				ng_modbus_send_response(priv, priv->lower,
				    pdu, len, tid, unit);
				callout_reset_sbt(&priv->client_timer,
				    SBT_1MS * MODBUS_CLIENT_TIMEOUT_MS, 0,
				    ng_modbus_client_timeout, node,
				    CALLOUT_RETURNUNLOCKED);
			}
			break;
		}

		case NGM_MODBUS_GET_RESPONSE: {
			struct ng_modbus_response *out;
			const struct modbus_pdu_buf *done;

			NG_MKRESPONSE(resp, msg, sizeof(priv->rsp), M_NOWAIT);
			if (resp == NULL) {
				error = ENOMEM;
				break;
			}
			out = (struct ng_modbus_response *)resp->data;
			out->state = priv->client.state;
			out->pdulen = 0;
			out->reserved = 0;

			done = modbus_client_response(&priv->client);
			if (done != NULL) {
				bcopy(done->buf, out->pdu, done->len);
				out->pdulen = done->len;
			}
			break;
		}

		case NGM_MODBUS_GET_BACKEND:
			NG_MKRESPONSE(resp, msg, sizeof(priv->be), M_NOWAIT);
			if (resp == NULL) {
				error = ENOMEM;
				break;
			}
			bcopy(&priv->be, resp->data, sizeof(priv->be));
			break;

		case NGM_MODBUS_SET_BACKEND: {
			struct modbus_backend_config backend;
			const struct ng_modbus_backend *nb;
			int berror;


			if (msg->header.arglen != sizeof(priv->be)) {
				error = EINVAL;
				break;
			}
			nb = (const struct ng_modbus_backend *)msg->data;
			backend.ncoils = nb->ncoils;
			backend.ndiscrete_in = nb->ndiscrete_in;
			backend.ninput_regs = nb->ninput_regs;
			backend.nholding_regs = nb->nholding_regs;

			/*
			 * Validate into a scratch area first, so a
			 * rejected size leaves the existing map untouched.
			 */
			berror = modbus_backend_init(&priv->newbe, &backend);
			if (berror != 0) {
				error = berror;
				break;
			}
			modbus_backend_destroy(&priv->backend);
			priv->backend = priv->newbe;
			priv->be = *nb;
			break;
		}

		case NGM_MODBUS_GET_STATS:
			NG_MKRESPONSE(resp, msg, sizeof(priv->stats), M_NOWAIT);
			if (resp == NULL) {
				error = ENOMEM;
				break;
			}
			bcopy(&priv->stats, resp->data, sizeof(priv->stats));
			break;

		case NGM_MODBUS_CLR_STATS:
			memset(&priv->stats, 0, sizeof(priv->stats));
			break;

		default:
			error = EINVAL;
			break;
		}
		break;

	default:
		error = EINVAL;
		break;
	}

done:
	NG_RESPOND_MSG(error, node, item, resp);
	NG_FREE_MSG(msg);

	return (error);
}

/*
 * A monotonic nanosecond stamp for the framings.  Bytes inside one item are
 * contiguous, so every byte of an item gets the same stamp; the gap between
 * two items is therefore real, which is what the t1.5 and t3.5 rules are
 * about.
 */
static uint64_t
ng_modbus_now_ns(void)
{
	struct timespec ts;

	getnanouptime(&ts);

	return ((uint64_t)ts.tv_sec * 1000000000ULL +
	    (uint64_t)ts.tv_nsec);
}

/*
 * Send one response back the way the request arrived.  The framing is the
 * node's own: the frameless transport returns the bare PDU, Modbus/TCP wraps
 * it in an MBAP header carrying the transaction identifier of the request,
 * RTU prefixes the address and appends the CRC, and ASCII adds the colon,
 * the LRC and CR LF.
 *
 * This runs in the receive callback, so the mbuf is allocated with M_NOWAIT
 * and a failure is counted rather than waited for.
 */
static void
ng_modbus_send_response(ng_modbus_p priv, hook_p hook, const uint8_t *pdu,
    uint16_t pdulen, uint16_t tid, uint8_t unit)
{
	uint8_t wire[MODBUS_ASCII_FRAME_MAXLEN];
	uint8_t frame[MODBUS_RTU_ADU_MAXLEN];
	struct mbuf *m;
	uint16_t crc;
	size_t n;
	int error;

	if (hook == NULL || NG_HOOK_PEER(hook) == NULL)
		return;

	switch (priv->cfg.transport) {
	case NG_MODBUS_TRANSPORT_TCP:
		n = modbus_tcp_encode(wire, sizeof(wire), tid, unit, pdu,
		    pdulen);
		break;

	case NG_MODBUS_TRANSPORT_RTU:
		if (pdulen + 3 > MODBUS_RTU_ADU_MAXLEN)
			return;
		frame[0] = unit;
		bcopy(pdu, frame + 1, pdulen);
		crc = modbus_crc16(frame, (size_t)pdulen + 1);
		frame[pdulen + 1] = (uint8_t)(crc & 0xff);
		frame[pdulen + 2] = (uint8_t)(crc >> 8);
		n = (size_t)pdulen + 3;
		bcopy(frame, wire, n);
		break;

	case NG_MODBUS_TRANSPORT_ASCII:
		/* The ASCII frame carries the unit identifier like RTU. */
		if (pdulen + 1 > MODBUS_RTU_ADU_MAXLEN)
			return;
		frame[0] = unit;
		bcopy(pdu, frame + 1, pdulen);
		n = modbus_ascii_encode(frame, (size_t)pdulen + 1, wire,
		    sizeof(wire));
		break;

	default:
		bcopy(pdu, wire, pdulen);
		n = pdulen;
		break;
	}

	if (n == 0)
		return;

	m = m_getm2(NULL, n, M_NOWAIT, MT_DATA, M_PKTHDR);
	if (m == NULL) {
		priv->stats.errors++;
		return;
	}
	bcopy(wire, mtod(m, uint8_t *), n);

	if (NG_HOOK_PEER(hook) == NULL) {
		m_free(m);
		return;
	}

	NG_SEND_DATA_ONLY(error, NG_HOOK_PEER(hook), m);
	if (error != 0) {
		priv->stats.errors++;
		return;
	}

	priv->stats.tx_frames++;
	priv->stats.tx_bytes += n;
}

/*
 * Hand a decoded request PDU to the engine and answer it.  Returns the
 * exception code when one was produced, 0 for a normal response, or a
 * negative errno when the input was not a PDU at all.
 */
static int
ng_modbus_answer(ng_modbus_p priv, hook_p hook, const uint8_t *pdu,
    uint16_t pdulen, uint16_t tid, uint8_t unit)
{
	struct modbus_pdu_buf rsp;
	int error;

	error = modbus_handle(&priv->ctx, pdu, pdulen, &rsp);
	if (error < 0) {
		priv->stats.errors++;
		return (error);
	}
	if (error > 0)
		priv->stats.errors++;

	ng_modbus_send_response(priv, hook, rsp.buf, rsp.len, tid, unit);

	return (error);
}

/*
 * Modbus/TCP: the bytes of one ADU may arrive in any number of pieces, so
 * every byte goes through the stream parser and each completed ADU is
 * answered with its own transaction identifier.
 */
static int
ng_modbus_input_tcp(ng_modbus_p priv, hook_p hook, const uint8_t *data,
    size_t len)
{
	const uint8_t *pdu;
	uint16_t pdulen, tid;
	uint8_t unit;
	size_t i;
	int error = 0;

	for (i = 0; i < len; i++) {
		switch (modbus_tcp_input(&priv->tcp, data[i])) {
		case MODBUS_TCP_ACCEPTED:
			break;

		case MODBUS_TCP_ERROR:
			priv->stats.errors++;
			break;

		case MODBUS_TCP_FRAME:
			pdu = modbus_tcp_frame(&priv->tcp, &pdulen, &tid,
			    &unit);
			if (pdu == NULL)
				break;
			modbus_tcp_release(&priv->tcp);

			/* A request for another unit is not ours. */
			if (unit != priv->ctx.unit_id)
				break;

			error = ng_modbus_answer(priv, hook, pdu, pdulen,
			    tid, unit);
			break;
		}
	}

	return (error);
}

/*
 * RTU.  The parser is driven one byte at a time with a timestamp; the bytes
 * of one item are treated as arriving closer together than t1.5, and the
 * frame is recognised by its CRC rather than by waiting for the silence.
 */
static int
ng_modbus_input_serial(ng_modbus_p priv, hook_p hook, const uint8_t *data,
    size_t len, uint32_t t15_ns)
{
	const uint8_t *frame;
	uint16_t framelen;
	size_t i;
	uint64_t now;

	now = ng_modbus_now_ns();

	for (i = 0; i < len; i++) {
		enum modbus_rtu_result r;

		r = modbus_rtu_input(&priv->rtu, data[i], now, t15_ns);
		if (r == MODBUS_RTU_DISCARDED)
			priv->stats.errors++;
		if (r != MODBUS_RTU_FRAME) {
			/*
			 * A frame in progress is delimited by silence, so
			 * arm the gap now rather than waiting for a byte
			 * that may never come.
			 */
			if (priv->rtu.state == MODBUS_RTU_RECEIVING)
				callout_reset_sbt(&priv->rtu_timer,
				    modbus_rtu_t35_ns(priv->baud,
				    priv->bits_per_char), 0,
				    ng_modbus_rtu_gap_callout,
				    priv->node, CALLOUT_RETURNUNLOCKED);
			continue;
		}

		frame = modbus_rtu_frame(&priv->rtu, &framelen);
		if (frame == NULL)
			continue;
		callout_stop(&priv->rtu_timer);

		/*
		 * Section 2.2 of the Serial Line guide: address 0 is a
		 * broadcast, which is executed but never answered.
		 */
		if (frame[0] == priv->ctx.unit_id &&
		    frame[0] != MODBUS_UNIT_BROADCAST)
			(void)ng_modbus_answer(priv, hook, frame + 1,
			    framelen - 3, 0, frame[0]);

		modbus_rtu_release(&priv->rtu);
	}

	return (0);
}

static int
ng_modbus_input_ascii(ng_modbus_p priv, hook_p hook, const uint8_t *data,
    size_t len)
{
	const uint8_t *frame;
	uint16_t framelen;
	size_t i;

	for (i = 0; i < len; i++) {
		enum modbus_ascii_result r;

		r = modbus_ascii_input(&priv->ascii, data[i]);
		if (r == MODBUS_ASCII_ERROR) {
			priv->stats.errors++;
			continue;
		}
		if (r != MODBUS_ASCII_FRAME)
			continue;

		frame = modbus_ascii_frame(&priv->ascii, &framelen);
		if (frame != NULL && frame[0] == priv->ctx.unit_id &&
		    frame[0] != MODBUS_UNIT_BROADCAST) {
			(void)ng_modbus_answer(priv, hook, frame + 1,
			    framelen - 2, 0, frame[0]);
		}
		/*
		 * A frame is released whether or not it was addressed
		 * to us, or the next characters would extend it.
		 */
		modbus_ascii_release(&priv->ascii);
	}

	return (0);
}

/*
 * Data path.  One item is fed to the configured framing, which decides where
 * the frames are; the answer is a PDU that goes back the same way.
 */
static int
ng_modbus_rcvdata(hook_p hook, item_p item)
{
	uint8_t buffer[MODBUS_ASCII_FRAME_MAXLEN];
	ng_modbus_p priv;
	const uint8_t *data;
	struct mbuf *m;
	size_t datalen;
	uint32_t t15_ns;

	NGI_GET_M(item, m);
	NG_FREE_ITEM(item);

	priv = NG_HOOK_PRIVATE(hook);
	if (priv == NULL || m == NULL) {
		NG_FREE_M(m);
		return (EINVAL);
	}

	priv->stats.rx_frames++;
	priv->stats.rx_bytes += m->m_pkthdr.len;

	/*
	 * A stream peer delivers its data as a chain whose first mbuf may be
	 * empty, so the framers are fed the whole chain rather than the
	 * first mbuf.  The length is known in advance from the packet
	 * header and is bounded, so the copy cannot fail.
	 */
	datalen = m->m_pkthdr.len;
	if (datalen == 0) {
		NG_FREE_M(m);
		return (0);
	}
	if (datalen > sizeof(buffer)) {
		priv->stats.errors++;
		NG_FREE_M(m);
		return (0);
	}
	m_copydata(m, 0, datalen, buffer);
	NG_FREE_M(m);
	data = buffer;

	switch (priv->cfg.transport) {
	case NG_MODBUS_TRANSPORT_TCP:
		(void)ng_modbus_input_tcp(priv, hook, data, datalen);
		break;

	case NG_MODBUS_TRANSPORT_RTU:
		t15_ns = modbus_rtu_t15_ns(priv->baud, priv->bits_per_char);
		(void)ng_modbus_input_serial(priv, hook, data, datalen,
		    t15_ns);
		break;

	case NG_MODBUS_TRANSPORT_ASCII:
		(void)ng_modbus_input_ascii(priv, hook, data, datalen);
		break;

	case NG_MODBUS_TRANSPORT_NONE:
	case NG_MODBUS_TRANSPORT_VNET:
	default:
		/*
		 * The frameless transport used for testing carries one whole
		 * PDU per item.
		 */
		(void)ng_modbus_answer(priv, hook, data, datalen, 0,
		    priv->ctx.unit_id);
		break;
	}

	return (0);
}

/*
 * Node destructor.  The framework calls this after every hook has been
 * destroyed and the input queue has been flushed, so nothing else may be
 * referencing the node at this point.
 */
static int
ng_modbus_shutdown(node_p node)
{
	ng_modbus_p priv;

	priv = NG_NODE_PRIVATE(node);
	NG_NODE_SET_PRIVATE(node, NULL);

	if (priv != NULL) {
		/*
		 * Drain rather than stop: a running handler may already be
		 * on its way back into the node, and shutdown must not
		 * return while one can still execute.
		 */
		callout_drain(&priv->client_timer);
		callout_drain(&priv->rtu_timer);
		modbus_backend_destroy(&priv->backend);
		free(priv, M_NETGRAPH_MODBUS);
	}

	NG_NODE_UNREF(node);

	return (0);
}
