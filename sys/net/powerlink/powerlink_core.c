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
 * The POWERLINK instance.  See powerlink_core.h for the locking rules.
 */

#include <sys/param.h>
#include <sys/counter.h>
#include <sys/kernel.h>
#include <sys/mutex.h>
#include <sys/mtx.h>
#include <sys/string.h>
#include <sys/time.h>

#include <net/powerlink/powerlink.h>
#include <net/powerlink/powerlink_core.h>
#include <net/powerlink/powerlink_edrv.h>
#include <net/powerlink/powerlink_edrv_ng.h>
#include <net/powerlink/powerlink_rt.h>

MALLOC_DEFINE(M_POWERLINK, "powerlink", "openPOWERLINK instance");

/*
 * The single instance.  Guarded by its own lock rather than by the instance
 * lock, because it has to be readable before an instance exists.
 */
static struct mtx powerlink_global_lock;
static struct powerlink_instance *powerlink_instance;
static int powerlink_global_ready;

void
powerlink_global_init(void)
{

	if (powerlink_global_ready)
		return;
	mtx_init(&powerlink_global_lock, "pl_global", NULL, MTX_DEF);
	powerlink_global_ready = 1;
}

void
powerlink_global_fini(void)
{

	if (!powerlink_global_ready)
		return;
	powerlink_instance_destroy();
	mtx_destroy(&powerlink_global_lock);
	powerlink_global_ready = 0;
}

void
powerlink_config_defaults(struct powerlink_config *cfg)
{

	memset(cfg, 0, sizeof(*cfg));
	cfg->mode = POWERLINK_MODE_MN;
	cfg->cycle_len_us = 1000;		/* 1 ms */
	cfg->loss_tol_us = 3000;		/* three cycles */
	cfg->node_id = 1;
	cfg->cpu = -1;				/* unpinned */
}

int
powerlink_config_check(const struct powerlink_config *cfg)
{

	if (cfg->mode != POWERLINK_MODE_MN && cfg->mode != POWERLINK_MODE_CN)
		return (EINVAL);
	if (cfg->cycle_len_us < POWERLINK_MIN_CYCLEN_US ||
	    cfg->cycle_len_us > POWERLINK_MAX_CYCLEN_US)
		return (EINVAL);
	if (cfg->loss_tol_us == 0)
		return (EINVAL);
	if (cfg->node_id == 0 || cfg->node_id > 247)
		return (EINVAL);
	if (cfg->cpu < -1)
		return (EINVAL);
	if (cfg->ifname[0] != '\0' &&
	    strlen(cfg->ifname) >= POWERLINK_MAX_IFNAMELEN)
		return (EINVAL);

	return (0);
}

/*
 * counter_u64_t is a pointer to a per-CPU counter, so the counters are
 * allocated when the instance is created and released when it goes away.
 * The data path adds to them without taking a lock, which is why the cycle
 * and receive paths do not contend with the control plane.
 */
static int
powerlink_instance_alloc_counters(struct powerlink_instance *inst)
{
	int i;
	counter_u64_t *slots;
	size_t n;

	n = sizeof(inst->counters) / sizeof(inst->counters.rx_frames);
	slots = (counter_u64_t *)&inst->counters;

	for (i = 0; i < (int)n; i++) {
		slots[i] = counter_u64_alloc(M_WAITOK);
		if (slots[i] == NULL) {
			while (i-- > 0)
				counter_u64_free(slots[i]);
			return (ENOMEM);
		}
		counter_u64_zero(slots[i]);
	}

	return (0);
}

static void
powerlink_instance_free_counters(struct powerlink_instance *inst)
{
	int i;
	counter_u64_t *slots;
	size_t n;

	n = sizeof(inst->counters) / sizeof(inst->counters.rx_frames);
	slots = (counter_u64_t *)&inst->counters;

	for (i = 0; i < (int)n; i++) {
		counter_u64_free(slots[i]);
		slots[i] = NULL;
	}
}

int
powerlink_instance_create(const struct powerlink_config *cfg)
{
	struct powerlink_instance *inst;
	struct powerlink_config local;
	int error;

	error = powerlink_config_check(cfg);
	if (error != 0)
		return (error);

	inst = malloc(sizeof(*inst), M_POWERLINK, M_WAITOK | M_ZERO);
	if (inst == NULL)
		return (ENOMEM);

	powerlink_config_defaults(&local);
	local.mode = cfg->mode;
	local.cycle_len_us = cfg->cycle_len_us;
	local.loss_tol_us = cfg->loss_tol_us;
	local.node_id = cfg->node_id;
	local.cpu = cfg->cpu;
	memcpy(local.ifname, cfg->ifname, sizeof(local.ifname));

	mtx_init(&inst->lock, "pl_inst", NULL, MTX_DEF);
	inst->cfg = local;
	inst->state = POWERLINK_STOPPED;
	inst->refcount = 1;

	error = powerlink_instance_alloc_counters(inst);
	if (error != 0) {
		mtx_destroy(&inst->lock);
		free(inst, M_POWERLINK);
		return (error);
	}

	error = powerlink_rt_create(&inst->rt);
	if (error != 0) {
		powerlink_instance_free_counters(inst);
		mtx_destroy(&inst->lock);
		free(inst, M_POWERLINK);
		return (error);
	}

	error = powerlink_edrv_ng_init(&inst->edrv, inst);
	if (error != 0) {
		powerlink_rt_destroy(&inst->rt);
		powerlink_instance_free_counters(inst);
		mtx_destroy(&inst->lock);
		free(inst, M_POWERLINK);
		return (error);
	}

	mtx_lock(&powerlink_global_lock);
	if (powerlink_instance != NULL) {
		/* openPOWERLINK is single instance; say so rather than lie. */
		mtx_unlock(&powerlink_global_lock);
		powerlink_edrv_ng_destroy(&inst->edrv);
		powerlink_rt_destroy(&inst->rt);
		powerlink_instance_free_counters(inst);
		mtx_destroy(&inst->lock);
		free(inst, M_POWERLINK);
		return (EBUSY);
	}
	powerlink_instance = inst;
	mtx_unlock(&powerlink_global_lock);

	return (0);
}

struct powerlink_instance *
powerlink_instance_get(void)
{
	struct powerlink_instance *inst;

	mtx_lock(&powerlink_global_lock);
	inst = powerlink_instance;
	if (inst != NULL)
		inst->refcount++;
	mtx_unlock(&powerlink_global_lock);

	return (inst);
}

void
powerlink_instance_put(struct powerlink_instance *inst)
{
	int dead;

	if (inst == NULL)
		return;

	mtx_lock(&powerlink_global_lock);
	dead = (--inst->refcount == 0);
	mtx_unlock(&powerlink_global_lock);

	if (!dead)
		return;

	/*
	 * The last reference is gone.  Stop the cycle first: it is the only
	 * thing that can be running against this state.
	 */
	powerlink_instance_stop(inst);

	mtx_lock(&powerlink_global_lock);
	if (powerlink_instance == inst)
		powerlink_instance = NULL;
	mtx_unlock(&powerlink_global_lock);

	powerlink_edrv_ng_destroy(&inst->edrv);
	powerlink_rt_destroy(&inst->rt);
	powerlink_instance_free_counters(inst);
	mtx_destroy(&inst->lock);
	free(inst, M_POWERLINK);
}

int
powerlink_instance_active(void)
{
	struct powerlink_instance *inst;

	mtx_lock(&powerlink_global_lock);
	inst = powerlink_instance;
	mtx_unlock(&powerlink_global_lock);

	return (inst != NULL);
}

void
powerlink_instance_destroy(void)
{
	struct powerlink_instance *inst;

	mtx_lock(&powerlink_global_lock);
	inst = powerlink_instance;
	mtx_unlock(&powerlink_global_lock);

	if (inst != NULL)
		(void)powerlink_instance_put(inst);
}

/*
 * One cycle.  The protocol engine is not in the tree yet, so the cycle
 * counts itself and returns; everything above this call is already wired.
 *
 * Context: real time thread.  No sleeping, no allocation, no netgraph.
 */
static int
powerlink_cycle(void *ctx, uint64_t now_ns, uint64_t deadline_ns)
{
	(void)ctx;
	(void)now_ns;
	(void)deadline_ns;

	return (0);
}

int
powerlink_instance_start(struct powerlink_instance *inst)
{
	int error;

	mtx_lock(&inst->lock);
	if (inst->state != POWERLINK_STOPPED) {
		error = EBUSY;
		goto out;
	}

	inst->state = POWERLINK_STOPPING;
	mtx_unlock(&inst->lock);

	error = powerlink_rt_start(&inst->rt, powerlink_cycle, inst,
	    inst->cfg.cycle_len_us, inst->cfg.cpu, 0);
	if (error != 0) {
		mtx_lock(&inst->lock);
		inst->state = POWERLINK_STOPPED;
		mtx_unlock(&inst->lock);
		return (error);
	}

	mtx_lock(&inst->lock);
	inst->state = POWERLINK_RUNNING;
	mtx_unlock(&inst->lock);

	return (0);
out:
	mtx_unlock(&inst->lock);
	return (error);
}

void
powerlink_instance_stop(struct powerlink_instance *inst)
{

	mtx_lock(&inst->lock);
	if (inst->state == POWERLINK_STOPPED) {
		mtx_unlock(&inst->lock);
		return;
	}
	inst->state = POWERLINK_STOPPING;
	mtx_unlock(&inst->lock);

	powerlink_rt_stop(&inst->rt);

	mtx_lock(&inst->lock);
	inst->state = POWERLINK_STOPPED;
	mtx_unlock(&inst->lock);
}

int
powerlink_instance_set_config(struct powerlink_instance *inst,
    const struct powerlink_config *cfg)
{
	int error;

	error = powerlink_config_check(cfg);
	if (error != 0)
		return (error);

	mtx_lock(&inst->lock);
	if (inst->state != POWERLINK_STOPPED) {
		/*
		 * Changing the cycle while it is running would mean
		 * rebuilding the real time path underneath it, so the
		 * caller stops, reconfigures and starts again.
		 */
		mtx_unlock(&inst->lock);
		return (EBUSY);
	}

	inst->cfg.mode = cfg->mode;
	inst->cfg.cycle_len_us = cfg->cycle_len_us;
	inst->cfg.loss_tol_us = cfg->loss_tol_us;
	inst->cfg.node_id = cfg->node_id;
	inst->cfg.cpu = cfg->cpu;
	memcpy(inst->cfg.ifname, cfg->ifname, sizeof(inst->cfg.ifname));
	mtx_unlock(&inst->lock);

	return (0);
}

void
powerlink_instance_get_config(struct powerlink_instance *inst,
    struct powerlink_config *cfg)
{

	mtx_lock(&inst->lock);
	*cfg = inst->cfg;
	mtx_unlock(&inst->lock);
}

enum powerlink_state
powerlink_instance_get_state(struct powerlink_instance *inst)
{
	enum powerlink_state state;

	mtx_lock(&inst->lock);
	state = inst->state;
	mtx_unlock(&inst->lock);

	return (state);
}

void
powerlink_instance_get_stats(struct powerlink_instance *inst,
    struct powerlink_stats *out)
{

	memset(out, 0, sizeof(*out));

	/*
	 * counter_u64_t is a pointer, so the values are read through the
	 * pointers the instance holds, not copied out of the struct.
	 */
	out->rx_frames = counter_u64_fetch(inst->counters.rx_frames);
	out->rx_bytes = counter_u64_fetch(inst->counters.rx_bytes);
	out->tx_frames = counter_u64_fetch(inst->counters.tx_frames);
	out->tx_bytes = counter_u64_fetch(inst->counters.tx_bytes);
	out->rx_dropped = counter_u64_fetch(inst->counters.rx_dropped);
	out->errors = counter_u64_fetch(inst->counters.errors);
}

void
powerlink_instance_clear_stats(struct powerlink_instance *inst)
{

	/*
	 * counter_u64_zero() zeroes one counter, but it does so on every
	 * CPU, so it has to be given the counter to clear: the inline form
	 * in machine/counter.h is the one that takes a value.
	 */
	counter_u64_zero(inst->counters.rx_frames);
	counter_u64_zero(inst->counters.rx_bytes);
	counter_u64_zero(inst->counters.tx_frames);
	counter_u64_zero(inst->counters.tx_bytes);
	counter_u64_zero(inst->counters.rx_dropped);
	counter_u64_zero(inst->counters.errors);
}

