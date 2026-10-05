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
 * The POWERLINK instance: one endpoint, owning everything that belongs to it.
 *
 * openPOWERLINK is single instance, so this module is too: there is at most
 * one instance, tracked here, and creating a second one fails with EBUSY
 * rather than pretending to work.
 *
 * Locking, in the order the layers take it:
 *
 *   powerlink_instance.lock	 protects the fields below and the reference
 *				 count; it is a sleepable mutex.
 *   rt.lock			 protects only the real time thread's own state
 *				 and its counters, and is a spin mutex: the
 *				 control plane never blocks on it for long.
 *
 * The data path takes no lock at all for its counters: they are counter(9)
 * counters, which are updated with a per-CPU add.
 */

#ifndef _SYS_NET_POWERLINK_POWERLINK_CORE_H_
#define	_SYS_NET_POWERLINK_POWERLINK_CORE_H_

#include <sys/param.h>
#include <sys/types.h>

#include <sys/counter.h>
#include <sys/mutex.h>
#include <sys/mtx.h>

#include <net/powerlink/powerlink.h>
#include <net/powerlink/powerlink_edrv.h>
#include <net/powerlink/powerlink_edrv_ng.h>
#include <net/powerlink/powerlink_rt.h>

enum powerlink_state {
	POWERLINK_STOPPED = 0,
	POWERLINK_STOPPING,
	POWERLINK_RUNNING
};

/* Module scoped state, set up and torn down by the module event handler. */
void	powerlink_global_init(void);
void	powerlink_global_fini(void);

/*
 * The counters themselves.  counter_u64_t is a pointer to a per-CPU counter,
 * so these are allocated with the instance and updated from the data path
 * without taking a lock.
 */
struct powerlink_counters {
	counter_u64_t	rx_frames;
	counter_u64_t	rx_bytes;
	counter_u64_t	tx_frames;
	counter_u64_t	tx_bytes;
	counter_u64_t	rx_dropped;
	counter_u64_t	errors;
};

/* A snapshot of them, as reported by the control interfaces. */
struct powerlink_stats {
	uint64_t	rx_frames;
	uint64_t	rx_bytes;
	uint64_t	tx_frames;
	uint64_t	tx_bytes;
	uint64_t	rx_dropped;
	uint64_t	errors;
};

struct powerlink_instance {
	struct mtx		 lock;
	struct powerlink_config	 cfg;
	enum powerlink_state	 state;
	int			 refcount;

	struct powerlink_edrv_ng	 edrv;
	struct powerlink_rt		 rt;
	struct powerlink_counters counters;

	uint8_t			 macaddr[6];
};

/*
 * Create the single instance, or fail with EBUSY if it already exists.  The
 * configuration is validated before anything is allocated.
 */
int	powerlink_instance_create(const struct powerlink_config *);
void	powerlink_instance_destroy(void);

/*
 * Take and drop a reference.  A caller that holds a pointer to the instance
 * across a possible destroy must hold one of these; netgraph nodes and open
 * device handles each hold one.
 */
struct powerlink_instance	*powerlink_instance_get(void);

/* Is an instance still alive?  The module refuses to unload if so. */
int	powerlink_instance_active(void);
void	powerlink_instance_put(struct powerlink_instance *);

/* Arm the cycle.  Safe from the control plane while the instance is held. */
int	powerlink_instance_start(struct powerlink_instance *);
void	powerlink_instance_stop(struct powerlink_instance *);

int	powerlink_instance_set_config(struct powerlink_instance *,
	    const struct powerlink_config *);
void	powerlink_instance_get_config(struct powerlink_instance *,
	    struct powerlink_config *);
enum powerlink_state powerlink_instance_get_state(struct powerlink_instance *);

void	powerlink_instance_get_stats(struct powerlink_instance *,
	    struct powerlink_stats *);
void	powerlink_instance_clear_stats(struct powerlink_instance *);

#endif /* _SYS_NET_POWERLINK_POWERLINK_CORE_H_ */
