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
 * The real time execution layer.
 *
 * Upstream runs the cycle from an EDRV timer callback.  This kernel has no
 * timer that wakes a thread at two hundred microseconds, so the cycle is
 * driven by a dedicated thread that owns its deadline: it sleeps until just
 * before the deadline, spends the last stretch reading the clock, runs the
 * cycle, and re-arms from an absolute deadline so the period does not drift.
 *
 * That is a soft real time arrangement.  The thread asks for the real time
 * scheduling class and may be pinned to a CPU, but neither is required and
 * neither makes a guarantee; the measured jitter is exported instead.
 */

#ifndef _SYS_NET_POWERLINK_POWERLINK_RT_H_
#define	_SYS_NET_POWERLINK_POWERLINK_RT_H_

#include <sys/param.h>
#include <sys/types.h>

#include <sys/condvar.h>
#include <sys/kthread.h>
#include <sys/mutex.h>
#include <sys/proc.h>

#include <net/powerlink/powerlink_edrv.h>

/*
 * How long before the deadline the thread stops sleeping and starts reading
 * the clock.  One tick is the floor under the sleep, so the spin has to cover
 * at least that much.
 */
#ifndef POWERLINK_RT_SPIN_NS
#define	POWERLINK_RT_SPIN_NS	1000000		/* 1 ms */
#endif

/* How long powerlink_rt_stop() waits for the cycle thread to leave. */
#define	POWERLINK_RT_STOP_WAIT_MS	1000

struct powerlink_rt_stats {
	uint64_t	 cycles;
	uint64_t	 deadline_misses;
	uint64_t	 wakeup_latency_max_ns;
	uint64_t	 cycle_duration_max_ns;
	uint64_t	 last_wakeup_ns;
	uint64_t	 last_start_ns;
};

enum powerlink_rt_state {
	POWERLINK_RT_STOPPED = 0,
	POWERLINK_RT_RUNNING,
	POWERLINK_RT_EXITED
};

/*
 * One cycle of the protocol stack, called once per cycle by the real time
 * thread.
 *
 * Context: real time thread, outside every lock this module holds.  It must
 * not sleep, must not allocate, must not block on a lock, and must not send
 * netgraph messages.  It returns non-zero when the cycle overran its own
 * budget, which the caller records as a deadline miss.
 */
typedef	int powerlink_cycle_fn(void *ctx, uint64_t now_ns,
	    uint64_t deadline_ns);

struct powerlink_rt {
	struct mtx		 lock;
	struct cv		 cv;
	volatile int		 state;
	struct thread		*thread;
	powerlink_cycle_fn	*cycle;
	void			*cycle_ctx;
	uint32_t		 cycle_len_ns;
	uint64_t		 next_deadline_ns;
	int			 cpu;
	int			 prio;
	struct powerlink_rt_stats stats;
};

int	powerlink_rt_create(struct powerlink_rt *);
void	powerlink_rt_destroy(struct powerlink_rt *);

/* Arm and disarm the cycle.  Both are safe to call from the control plane. */
int	powerlink_rt_start(struct powerlink_rt *, powerlink_cycle_fn *, void *,
	    uint32_t cycle_len_us, int cpu, int prio);
void	powerlink_rt_stop(struct powerlink_rt *);

void	powerlink_rt_get_stats(struct powerlink_rt *,
	    struct powerlink_rt_stats *);

static inline uint64_t
powerlink_now_ns(void)
{
	struct timespec ts;

	getnanouptime(&ts);

	return ((uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec);
}

#endif /* _SYS_NET_POWERLINK_POWERLINK_RT_H_ */
