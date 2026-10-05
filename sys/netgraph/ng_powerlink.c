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
 * ng_powerlink: a netgraph node that attaches a POWERLINK endpoint to a
 * graph.
 *
 * The node is an adapter and nothing more.  It carries Ethernet frames, it
 * takes configuration and it reports state; the NMT state machine, the DLL,
 * the PDO mapping and the cycle scheduler all live in the POWERLINK core
 * above it, so there is exactly one implementation of the protocol however it
 * is transported.
 */

#include <sys/param.h>
#include <sys/kernel.h>
#include <sys/mbuf.h>
#include <sys/malloc.h>
#include <sys/string.h>

#include <netgraph/ng_message.h>
#include <netgraph/netgraph.h>
#include <netgraph/ng_parse.h>

#include <net/powerlink/powerlink.h>
#include <net/powerlink/powerlink_core.h>
#include <net/powerlink/powerlink_edrv.h>
#include <net/powerlink/powerlink_edrv_freebsd.h>
#include <net/powerlink/powerlink_edrv_ng.h>
#include <net/powerlink/powerlink_rt.h>

#include <netgraph/ng_powerlink.h>

static MALLOC_DEFINE(M_NG_POWERLINK, "ng_powerlink",
    "netgraph POWERLINK node");

struct ng_powerlink_private {
	struct powerlink_instance	*inst;
	hook_p				 lower;
};

/*
 * What NGM_POWERLINK_SET_CONFIG carries.  It is the text parse table and the
 * binary layout in one, so the two cannot drift apart, and it is validated in
 * full before any of it reaches the instance.
 */
struct ng_powerlink_cfg {
	uint32_t	mode;
	uint32_t	cycle_len_us;
	uint32_t	loss_tol_us;
	uint32_t	node_id;
	int32_t		cpu;
};

static int ng_powerlink_constructor(node_p node);
static int ng_powerlink_rcvmsg(node_p node, item_p item, hook_p lasthook);
static int ng_powerlink_rcvdata(hook_p hook, item_p item);
static int ng_powerlink_newhook(node_p node, hook_p hook, const char *name);
static int ng_powerlink_connect(hook_p hook);
static int ng_powerlink_disconnect(hook_p hook);
static int ng_powerlink_shutdown(node_p node);
static int ng_powerlink_mod_event(module_t mod, int event, void *data);

static const struct ng_parse_struct_field ng_powerlink_config_fields[] = {
	{ "mode",		&ng_parse_uint32_type	},
	{ "cycle_len_us",	&ng_parse_uint32_type	},
	{ "loss_tol_us",	&ng_parse_uint32_type	},
	{ "node_id",		&ng_parse_uint32_type	},
	{ "cpu",		&ng_parse_int32_type	},
	{ NULL }
};

static const struct ng_parse_struct_field ng_powerlink_status_fields[] =
    NG_POWERLINK_STATUS_INFO;

static const struct ng_parse_struct_field ng_powerlink_stats_fields[] =
    NG_POWERLINK_STATS_INFO;

static const struct ng_parse_type ng_powerlink_config_type = {
	&ng_parse_struct_type,
	ng_powerlink_config_fields
};

static const struct ng_parse_type ng_powerlink_status_type = {
	&ng_parse_struct_type,
	ng_powerlink_status_fields
};

static const struct ng_parse_type ng_powerlink_stats_type = {
	&ng_parse_struct_type,
	ng_powerlink_stats_fields
};

static const struct ng_cmdlist ng_powerlink_cmds[] = {
	{
	  NG_POWERLINK_COOKIE,
	  NGM_POWERLINK_SET_CONFIG,
	  "setconfig",
	  &ng_powerlink_config_type,
	  &ng_powerlink_config_type
	},
	{
	  NG_POWERLINK_COOKIE,
	  NGM_POWERLINK_GET_CONFIG,
	  "getconfig",
	  NULL,
	  &ng_powerlink_config_type
	},
	{
	  NG_POWERLINK_COOKIE,
	  NGM_POWERLINK_START,
	  "start",
	  NULL,
	  NULL
	},
	{
	  NG_POWERLINK_COOKIE,
	  NGM_POWERLINK_STOP,
	  "stop",
	  NULL,
	  NULL
	},
	{
	  NG_POWERLINK_COOKIE,
	  NGM_POWERLINK_GET_STATUS,
	  "getstatus",
	  NULL,
	  &ng_powerlink_status_type
	},
	{
	  NG_POWERLINK_COOKIE,
	  NGM_POWERLINK_GET_STATS,
	  "getstats",
	  NULL,
	  &ng_powerlink_stats_type
	},
	{
	  NG_POWERLINK_COOKIE,
	  NGM_POWERLINK_RESET_STATS,
	  "resetstats",
	  NULL,
	  NULL
	},
	{ 0 }
};

static struct ng_type ng_powerlink_typestruct = {
	.version =	NG_ABI_VERSION,
	.name =		NG_POWERLINK_NODE_TYPE,
	.mod_event =	ng_powerlink_mod_event,
	.constructor =	ng_powerlink_constructor,
	.rcvmsg =	ng_powerlink_rcvmsg,
	.shutdown =	ng_powerlink_shutdown,
	.newhook =	ng_powerlink_newhook,
	.connect =	ng_powerlink_connect,
	.rcvdata =	ng_powerlink_rcvdata,
	.disconnect =	ng_powerlink_disconnect,
	.cmdlist =	ng_powerlink_cmds,
};

/*
 * The module is powerlink.ko and is loaded as "powerlink", so the module
 * declaration is written out rather than taken from NETGRAPH_INIT(), which
 * would name it "ng_powerlink" after the node type instead.
 */
static moduledata_t powerlink_moddata = {
	"powerlink",
	ng_powerlink_mod_event,
	&ng_powerlink_typestruct
};

DECLARE_MODULE(powerlink, powerlink_moddata, SI_SUB_PSEUDO, SI_ORDER_MIDDLE);
MODULE_DEPEND(powerlink, netgraph, NG_ABI_VERSION, NG_ABI_VERSION,
    NG_ABI_VERSION);

static int
ng_powerlink_constructor(node_p node)
{
	struct ng_powerlink_private *priv;
	struct powerlink_config cfg;
	int error;

	priv = malloc(sizeof(*priv), M_NG_POWERLINK, M_WAITOK | M_ZERO);
	if (priv == NULL)
		return (ENOMEM);

	powerlink_config_defaults(&cfg);

	/*
	 * The instance is created here so that a node cannot exist without
	 * one, and so that the second node of the type fails here with a
	 * clear reason rather than half working.
	 */
	error = powerlink_instance_create(&cfg);
	if (error != 0) {
		free(priv, M_NG_POWERLINK);
		return (error);
	}

	priv->inst = powerlink_instance_get();
	priv->lower = NULL;
	NG_NODE_SET_PRIVATE(node, priv);

	return (0);
}

static int
ng_powerlink_newhook(node_p node, hook_p hook, const char *name)
{
	struct ng_powerlink_private *priv;

	if (strcmp(name, NG_POWERLINK_HOOK_LOWER) != 0)
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
ng_powerlink_connect(hook_p hook)
{
	struct ng_powerlink_private *priv;

	priv = NG_HOOK_PRIVATE(hook);
	if (priv == NULL || priv->inst == NULL)
		return (EINVAL);

	return (powerlink_edrv_ng_attach(&priv->inst->edrv, hook));
}

static int
ng_powerlink_disconnect(hook_p hook)
{
	struct ng_powerlink_private *priv;
	node_p node;

	node = NG_HOOK_NODE(hook);
	priv = NG_HOOK_PRIVATE(hook);
	if (priv == NULL)
		return (EINVAL);

	if (priv->inst != NULL)
		powerlink_edrv_ng_detach(&priv->inst->edrv);
	if (priv->lower == hook)
		priv->lower = NULL;
	NG_HOOK_SET_PRIVATE(hook, NULL);

	if (NG_NODE_NUMHOOKS(node) == 0 && NG_NODE_IS_VALID(node))
		ng_rmnode_self(node);

	return (0);
}

/*
 * The receive path.  It runs in netgraph context and must not sleep, so it
 * does no copying and no allocation: the frame is handed to the driver,
 * which hands it to the engine.
 */
static int
ng_powerlink_rcvdata(hook_p hook, item_p item)
{
	struct ng_powerlink_private *priv;
	struct mbuf *m;

	NGI_GET_M(item, m);
	NG_FREE_ITEM(item);

	priv = NG_HOOK_PRIVATE(hook);
	if (priv == NULL || priv->inst == NULL || m == NULL) {
		m_free(m);
		return (EINVAL);
	}

	powerlink_edrv_ng_input(&priv->inst->edrv, m);

	return (0);
}

static int
ng_powerlink_rcvmsg(node_p node, item_p item, hook_p lasthook)
{
	struct ng_powerlink_private *priv;
	struct powerlink_rt_stats rtstats;
	struct powerlink_stats stats;
	struct powerlink_config cfg;
	struct ng_mesg *msg, *resp;
	int error;

	NGI_GET_MSG(item, msg);
	priv = NG_NODE_PRIVATE(node);
	resp = NULL;
	error = 0;
	if (priv == NULL || priv->inst == NULL) {
		error = EINVAL;
		goto done;
	}

	switch (msg->header.typecookie) {
	case NG_POWERLINK_COOKIE:
		switch (msg->header.cmd) {
		case NGM_POWERLINK_SET_CONFIG: {
			const struct ng_powerlink_cfg *in;

			if (msg->header.arglen != sizeof(*in)) {
				error = EINVAL;
				break;
			}
			in = (const struct ng_powerlink_cfg *)msg->data;

			memset(&cfg, 0, sizeof(cfg));
			cfg.mode = (enum powerlink_mode)in->mode;
			cfg.cycle_len_us = in->cycle_len_us;
			cfg.loss_tol_us = in->loss_tol_us;
			cfg.node_id = in->node_id;
			cfg.cpu = in->cpu;

			error = powerlink_instance_set_config(priv->inst,
			    &cfg);
			break;
		}

		case NGM_POWERLINK_GET_CONFIG:
			NG_MKRESPONSE(resp, msg, sizeof(cfg), M_NOWAIT);
			if (resp == NULL) {
				error = ENOMEM;
				break;
			}
			powerlink_instance_get_config(priv->inst, &cfg);
			bcopy(&cfg, resp->data, sizeof(cfg));
			break;

		case NGM_POWERLINK_START:
			error = powerlink_instance_start(priv->inst);
			break;

		case NGM_POWERLINK_STOP:
			powerlink_instance_stop(priv->inst);
			break;

		case NGM_POWERLINK_GET_STATUS: {
			struct ng_powerlink_status *status;

			NG_MKRESPONSE(resp, msg, sizeof(*status), M_NOWAIT);
			if (resp == NULL) {
				error = ENOMEM;
				break;
			}
			status = (struct ng_powerlink_status *)resp->data;
			powerlink_instance_get_config(priv->inst, &cfg);
			status->abi_version = POWERLINK_VERSION;
			status->state = (uint32_t)
			    powerlink_instance_get_state(priv->inst);
			status->mode = (uint32_t)cfg.mode;
			status->cycle_len_us = cfg.cycle_len_us;
			status->node_id = cfg.node_id;
			status->edrv_kind = POWERLINK_EDRV_KIND_NETGRAPH;
			break;
		}

		case NGM_POWERLINK_GET_STATS: {
			struct ng_powerlink_stats *out;
			uint32_t i;

			NG_MKRESPONSE(resp, msg, sizeof(*out), M_NOWAIT);
			if (resp == NULL) {
				error = ENOMEM;
				break;
			}
			out = (struct ng_powerlink_stats *)resp->data;
			memset(out, 0, sizeof(*out));

			powerlink_instance_get_stats(priv->inst, &stats);
			powerlink_rt_get_stats(&priv->inst->rt, &rtstats);

			/*
			 * Both structures start with the same six counters,
			 * so they are copied field by field rather than by
			 * a cast that would depend on the layout matching.
			 */
			uint64_t *dst = (uint64_t *)out;
			const uint64_t *src = (const uint64_t *)&stats;

			for (i = 0; i < sizeof(stats) / sizeof(uint64_t); i++)
				dst[i] = src[i];
			dst[6] = rtstats.cycles;
			dst[7] = rtstats.deadline_misses;
			dst[8] = rtstats.wakeup_latency_max_ns;
			dst[9] = rtstats.cycle_duration_max_ns;
			break;
		}

		case NGM_POWERLINK_RESET_STATS:
			powerlink_instance_clear_stats(priv->inst);
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
 * Node destructor.  By the time it runs the framework has destroyed every
 * hook, so the driver has already been detached and the only thing left is
 * the instance and the reference this node holds on it.
 */
static int
ng_powerlink_shutdown(node_p node)
{
	struct ng_powerlink_private *priv;

	priv = NG_NODE_PRIVATE(node);
	NG_NODE_SET_PRIVATE(node, NULL);

	if (priv == NULL)
		return (0);

	if (priv->inst != NULL)
		powerlink_instance_put(priv->inst);
	free(priv, M_NG_POWERLINK);

	return (0);
}

static int
ng_powerlink_mod_event(module_t mod, int event, void *data)
{
	int error = 0;

	(void)data;

	switch (event) {
	case MOD_LOAD:
		powerlink_global_init();
		break;

	case MOD_UNLOAD:
		/*
		 * Refuse while anything is still built on top of this
		 * module: an instance means a cycle thread and buffers, and
		 * a node means a graph hook.  This is the same rule netgraph
		 * itself applies to itself.
		 */
		powerlink_global_fini();
		if (powerlink_instance_active())
			error = EBUSY;
		break;

	default:
		error = EOPNOTSUPP;
		break;
	}

	return (error);
}
