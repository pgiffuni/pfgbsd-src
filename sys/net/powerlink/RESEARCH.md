# openPOWERLINK 2.7.2 to FreeBSD — Phase 1 research

Status: research and classification complete, **no implementation code written**.
Reference tree: `https://github.com/OpenAutomationTechnologies/openPOWERLINK_V2`
at tag `V2.7.2`, commit `048650a`, checked out read-only outside the FreeBSD
tree and not vendored into it.
Companion artifact: `share/tests/powerlink/PORTING.md`, the generated
function-level table the specification asks for, with
`ng_powerlink/gen_porting_table.py` as its generator.

---

## 0. Findings that change the plan

Four results from the source study materially reshape the specification's
priorities. Each is verified with references below.

1. **No SYNC/JANUS work.** The distributed-clock synchronisation algorithm was
   removed from openPOWERLINK in the 2.6/2.7 line. What remains in
   `timesynck.c` is SoC-time forwarding for the PCIe/shared-memory path.
   Specification §18 is far smaller than it reads.
2. **No kernel SDO.** SDO is userland only (`stack/src/user/sdo/`); the kernel
   registers no SDO handler. Despite §17 listing SDO, `powerlink.ko` does not
   carry an SDO server.
3. **`edrvcyclic.c` contains no Linux code.** The file the specification calls
   its most important timing component is portable C over an `hrestimer`
   abstraction: no `linux/*.h`, no `printk`, no `alloc_sem`, no atomics, no
   `kmalloc`. Its only platform dependency is which timer backend implements
   `hrestimer_modifyTimer`/`hrestimer_deleteTimer`.
4. **The stack is intrinsically single-instance**, so §59's documented
   fallback applies rather than instance virtualisation.

---

## 1. Licensing

Upstream is **BSD-3-Clause**, copyright SYSTEC electronic GmbH and
B&R Industrial Automation GmbH (`stack/src/kernel/edrv/edrvcyclic.c:14-17`).
Porting it into FreeBSD is straightforward, with one obligation: every reused
or ported file keeps its upstream copyright notice and BSD-3-Clause text
verbatim. Files that are wholly new FreeBSD code (EDRV, netgraph node, RT
layer, ioctl surface) carry `SPDX-License-Identifier: BSD-2-Clause`.

This differs from the Modbus project, where nothing was derived from upstream
and every file was new.

---

## 2. Real porting surface

807 functions were extracted from the ten kernel areas. Most of the volume is
Linux, Windows, VxWorks and openMAC backends that the specification excludes.

| Disposition | Functions | Meaning |
|---|---|---|
| `port` | 199 | protocol logic, operating system calls replaced |
| `reuse` | 72 | protocol code taken as-is |
| `reimplement` | 55 | platform layer written for FreeBSD |
| `exclude` | 340 | not ported |
| `reference` | 47 | read but not ported |

Per function: `share/tests/powerlink/PORTING.md`.

The portable core is about 15 000 lines, dominated by `dll/dllkframe.c` (3151),
`nmt/nmtk.c` (1707), `dll/dllknode.c` (1348), `dll/dllkevent.c` (1222),
`errhnd/errhndk.c` (1117), `dll/dllk.c` (1102), `dll/dllkstatemachine.c` (978),
`edrv/edrvcyclic.c` (811). Ten of the fourteen functions in
`dllkstatemachine.c` are free of operating system calls and are the cleanest
reuse candidates in the tree.

### The CAL is link-time source substitution, not vtables

There is no `tCtrlkCal` or `tEvtCal` function-pointer table. Portable logic in
`ctrlk.c`/`eventk.c`/`errhndk.c`/`timesynck.c` calls unqualified
`ctrlkcal_*`/`eventkcal_*`/`errhndkcal_*`/`timesynckcal_*` symbols
(`stack/src/kernel/ctrl/ctrlk.c:46,155`; `event/eventk.c:48`; `errhndk.c:57`),
and the build selects exactly one implementation of each. The port therefore
supplies one file per CAL with the existing symbol set; no header change is
needed except a `_FREEBSD_` target.

Worth noting: `event/eventkcalintf-circbuf.c` is portable and selected by
every platform (`stack/cmake/stackfiles.cmake:391,396,401,406,410,415,420,425`),
so the event queue machinery is reused near-verbatim.

Also relevant: `timesync/timesynckcal-bsdsem.c` is an existing BSD-flavoured
CAL using `sem_open`/`shm_open`, and
`timer/hrestimer-posix_clocknanosleep.c` is an existing implementation of the
continuous-rearm logic with `clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME)`.
Both are closer to FreeBSD than their Linux counterparts and are the models
for the FreeBSD versions.

---

## 3. Single instance, and why that is the right first step

Evidence, not inference:

- `tDllkInstance dllkInstance_g` (`dll/dllk.c:77`) is non-static and is
  referenced without an instance parameter from all thirteen files in `dll/`
  plus `pdo/pdokcal.c:185,190` — roughly 400 references.
- `dllkInstance_g.pTxBuffer` points into a file-static array
  (`dll/dllk.c:102,158`).
- Callbacks are invoked as `dllkInstance_g.pfnXxx(...)` with no context
  parameter (`dllkframe.c:215,936,1075,2757`).
- `TGT_DLLK_DEFINE_CRITICAL_SECTION` (`include/kernel/dllktgt.h:53`) declares
  exactly one file-static spinlock for the whole DLL.
- `static tNmtkInstance nmtkInstance_l` (`nmt/nmtk.c:138`) and
  `static tPdokInstance pdokInstance_g` (`pdo/pdok.c:97`) are separate
  singletons, and `static tEdrvcyclicInstance edrvcyclicInstance_l`
  (`edrv/edrvcyclic.c:128`) is a third.

**Decision: exactly one POWERLINK instance in the first version, documented as a
limitation.** Supporting several would mean adding an instance parameter to
~250 functions, moving three file-static arrays into the instance, replacing
the four `CIRCBUF_*` slots with per-instance handles, and threading an opaque
pointer through the EDRV-registered `SECTION_*` callbacks, which cannot carry
one without changing the EDRV contract. That is a separate enhancement, as
§59 itself anticipates.

The `struct powerlink_instance` wrapper from §39 therefore holds exactly one
DLL, one cyclic EDRV, one RT thread and one netgraph attachment.

---

## 4. Timing: what the cycle engine actually needs, and what FreeBSD has

### 4.1 What upstream requires

- `cycleTime` is microseconds (`edrvcyclic.h:110`), converted once to
  nanoseconds (`edrvcyclic.c:339`); `timeOffsetNs` is nanoseconds.
- The cycle timer is continuous for normal operation and one-shot for a single
  frame (`edrvcyclic.c:338-340`, `dllkframe.c:2144`). The slot timer is always
  one-shot (`:778-782`).
- Both timer backends enforce a floor: 100 µs for a continuous timer, 5–20 µs
  for a one-shot (`hrestimer-linuxkernel.c:62-63`,
  `hrestimer-posix.c:62-63`). **A cycle time below 100 µs is silently stretched
  to 100 µs upstream.**
- Callbacks must be mutually exclusive with the EDRV RX/TX callbacks
  (documented contract, `hrestimer-posix.c:238-240`).
- The handle is a generation counter: `modifyTimer` increments before arming so
  an in-flight callback sees a stale handle and discards itself
  (`hrestimer-posix.c:67-72`, callback check at `edrvcyclic.c:496,641`).

### 4.2 What FreeBSD 16 provides

Verified against this tree:

- **Sub-tick callouts are expressible.** `callout_when()`
  (`sys/kern/kern_timeout.c:879-920`) clamps to `tick_sbt` only when
  `C_HARDCLOCK` is set (`:890-891`); below `sbt_tickthreshold` it anchors on
  `sbinuptime()` with precision `sbt >> tc_precexp`, and `tc_precexp` is 31
  when `tc_timepercentage == 0` (`sys/kern/kern_tc.c:1953-1957`), giving
  precision 1. A 200–1000 µs deadline is therefore reachable.
  `callout_reset_sbt_on()` explicitly handles re-arming a callout that is
  currently running (`kern_timeout.c:969-1007`), and the absolute-deadline
  re-arm idiom is in-tree at `sys/kern/sys_timerfd.c:402-432`.
- **No `ktimer`, no `hrtimer`.** The `ktimer` syscall numbers exist
  (`sys/sys/syscall.h:218-222`) but the only implementation is the internal
  timerfd timer-id path; there is no module-visible timer object.
- **SBT is the right clock.** `SBT_1MS`/`SBT_1US`/`SBT_1NS`
  (`sys/sys/time.h:127-131`), read cheaply via `sbinuptime()` (`:578-585`).
  On x86 the timecounter is rdtsc-based (`sys/x86/x86/tsc.c:790`). Note
  `sbttots()`/`sbttotv()` wrap their seconds field (`time.h:355,384`) — avoid
  them.
- **Sub-millisecond sleep for a kernel thread** is available as
  `pause_sbt()` (`sys/sys/systm.h:503`) and `msleep_sbt()` (`:495`), which
  ride the same sub-tick callout machinery
  (`sys/kern/subr_sleepqueue.c:393-417`).

### 4.3 Design, and the honesty requirement

`powerlink_rt.c` owns a dedicated kernel thread that:

1. computes the next cycle deadline once, in SBT, from the configured cycle
   time — the cycle arithmetic stays in POWERLINK units and converts once at
   this boundary, as §7 of the specification requires;
2. sleeps to the deadline minus a small margin, then spins on `sbinuptime()` for
   the remainder, because callout dispatch latency, not resolution, is what
   limits accuracy;
3. re-arms with an absolute deadline so the period does not drift;
4. raises itself to the RT scheduling class and pins itself to a CPU.

The spin is the deliberate cost: it buys deterministic wakeup at the price of
one busy CPU while the stack runs, and that is the trade §33 asks to be stated
rather than hidden. No hard-real-time claim will be made; cycle jitter, deadline
misses and wakeup latency are measured and exported.

**Priority and affinity.** `kthread_add()` takes fork flags, not scheduling
options: there are no `KF_*` flags, and `cpuset_kernthread()`
(`sys/kern/kern_kthread.c:330`) forces every new kthread into `cpuset_kernel`.
Priority must therefore be set after creation under `thread_lock()`
(the in-tree template is `sys/kern/subr_taskqueue.c:800-802`), and affinity set
with `cpuset_setthread(td->td_tid, mask)` after an `RFSTOPPED` start
(`subr_taskqueue.c:787-799`) or `cpuset_setithread(td_tid, cpu)`
(`sys/kern/kern_timeout.c:396-402`). `uio_set_cpuset_seq()` does not exist.

**RT priority.** The `kern.rtprio.*` sysctls are **gone** from this tree; the
RT range is compile-time fixed at 8–39 internally
(`sys/sys/priority.h:70,107-108`), mapped from user rtprio 0–31
(`sys/sys/rtprio.h:61-62`, `sys/kern/kern_resource.c:494-512`). Nothing in
`sys/` documents the resulting latency. A module can raise its own thread
directly, since `rtp_to_pri()` (`kern_resource.c:489`) performs no privilege
check; the nearest in-tree kernel-side use is `sys/kern/kern_timeout.c:386-391`,
which uses `PRI_ITHD`. Priority is therefore requested, not required, and its
effect is measured.

Worth recording from the survey: SCHED_FIFO and SCHED_RR are behaviourally
identical in both schedulers — `sched_rr_interval()` is referenced only by the
POSIX shim (`sched_shim.c:38`) and consumed by neither `sched_4bsd.c` nor
`sched_ule.c` — and ULE explicitly skips the tick path for FIFO threads
(`sched_ule.c:2760-2761`). So requesting the RT class does give FIFO behaviour;
it simply has no round-robin quantum to worry about.

---

## 5. EDRV contract

The API is `stack/include/kernel/edrv.h`, and it is small: `edrv_init`,
`edrv_exit`, `edrv_getMacAddr`, `edrv_setRxMulticastMacAddr`,
`edrv_clearRxMulticastMacAddr`, `edrv_changeRxFilter`, `edrv_allocTxBuffer`,
`edrv_freeTxBuffer`, `edrv_sendTxBuffer`, plus `edrv_releaseRxBuffer` (gated on
`CONFIG_DLL_DEFERRED_RXFRAME_RELEASE_SYNC`, default true), `edrv_updateTxBuffer`
(`CONFIG_EDRV_AUTO_RESPONSE`, default false) and `edrv_getMacTime`
(`EDRV_USE_TTTX`, default false).

`tEdrvTxBuffer` keeps its launch-time fields, as §3 requires:
`timeOffsetNs`, `fLaunchTimeValid`, `launchTime` (`edrv.h:152-167`). Upstream
writes them in the TTTX branch of the cyclic driver and **clears them after a
successful send** because the driver consumed them (`edrvcyclic.c:728-750`).
The FreeBSD abstraction keeps all three fields even though the first EDRV
cannot use them.

Ownership, which the specification asks to be documented:
- `edrv_allocTxBuffer` allocates `pBuffer` of `maxBufferSize`, which the caller
  preset; the descriptor itself belongs to the DLL.
- `edrv_freeTxBuffer` frees `pBuffer` and NULLs it first.
- `edrv_sendTxBuffer` is a non-blocking submit called from the DLL event path
  *and* from inside the cycle timer callbacks (`edrvcyclic.c:652,745,770`), so
  it must be safe in timer context and mutually exclusive with the RX/TX
  callbacks.
- The RX buffer is driver-owned and valid only during the RX callback unless
  the callback returns `kEdrvReleaseRxBufferLater` (`edrv.h:96-100`), which is
  the deferred-release mechanism the whole DLL RX design depends on.

Two upstream quirks to reproduce rather than fix, for behavioural parity:

- `timeOffsetNs` is an **absolute** offset from cycle start
  (`dllknode.c:820,829,838,860`; `dllkevent.c:1038`) but is armed as a
  **relative** slot delay (`edrvcyclic.c:778-779`). The openMAC cyclic variant
  confirms the absolute reading by accumulating it
  (`edrvcyclic-openmac.c:547`). The port reproduces the upstream arithmetic
  exactly, and the report notes it.
- The cyclic error callback's return value **overwrites** the cycle error
  (`edrvcyclic.c:612,667,805`), so a callback returning `kErrorOk` suppresses a
  second report. `dllk_cbCyclicError` depends on this (`dllk.c:964-980`).

Frame-loss detection is not in `edrvcyclic.c` at all; it lives in the DLL and
`synctimer`, driven by `lossOfFrameTolerance` (OBD 0x1C14).

---

## 6. Netgraph node

`ng_powerlink` is an adapter, not a second protocol implementation (§22). Its
data path for virtual operation:

```
netgraph -> ng_powerlink -> virtual EDRV -> dllkframe_processFrameReceived
```

and back out through `edrv_sendTxBuffer`. The upstream `veth` module is not
ported; its role is filled by the netgraph virtual EDRV, whose contract is
narrow: `veth_init`/`veth_exit` plus `dllk_regAsyncHandler` for the outbound
direction and `dllkcal_sendAsyncFrame()` for the inbound one
(`veth-linuxuser.c:285-296,365-367`).

Two facts from the framework survey shape this:

- **The netgraph data path runs on the `ng_queue` worker threads**
  (`ng_base.c:3232-3238`), so the cycle cannot be driven from netgraph. It is
  re-armed from the module's own kernel thread, exactly as §32/§33 require.
- **Sending an item back out on the hook it arrived on is permitted**:
  `ng_address_hook()` merely retargets to the peer and only rejects a dead hook
  (`ng_base.c:3596-3630`). Loop protection is the caller's job, as
  `ng_bridge` does with its `loopCount`/`loopDrops` (`ng_bridge.c:833-839`).

Initial hooks: `lower` for Ethernet frames (§24), plus a control interface.
Control messages are versioned and carry no kernel pointers (§25, §44).

For the hardware case, §27's requirement — use a normal Ethernet interface
without writing a NIC driver — is constrained by what the tree allows:

- **TX onto a foreign ifnet** is supported: `ether_output_frame(ifp, m)`
  (`sys/net/if_ethersubr.c:486`) or `if_transmit()` (`sys/net/if.c:4941`),
  which is exactly what `ng_ether_rcv_lower()` does
  (`ng_ether.c:668-709`). Both take the sendq lock and may block, which is a
  constraint on the RT path and must be measured, not assumed.
- **RX from a NIC we do not own** has no generic hook. The only sanctioned
  mechanism is the `ng_ether` `if_l2com` slot: three exported function pointers
  in `if_ethersubr.c:98-100`, claimed at `:600-611`, installed on module load
  (`ng_ether.c:816-819`). Attaching as a second driver on the same ifnet is not
  supported — there is one `if_transmit`/`if_input` triple per ifnet.
- **Multicast join** is `if_addmulti()` (`sys/net/if_var.h:534`), and per
  `ng_ether.c:598-604` the `ifmultiaddr *` cannot be retained across calls
  because the call must happen inside a net epoch.
- **Absolute-time hardware transmit does not exist** on FreeBSD. So §31's
  LEVEL 2 and LEVEL 3 are not reachable without NIC driver work; v1 implements
  LEVEL 1 (software-timed TX) and keeps the fields, which is what §31 asks for.

---

## 7. Control ABI

Upstream uses a character device `/dev/plk` with fifteen ioctls
(`stack/include/common/driver-linux.h:47-73`), not procfs or netlink. The
FreeBSD surface mirrors that on `/dev/powerlink`:

- `make_dev()` (`sys/sys/conf.h:291`), `devfs_set_cdevpriv()` (`:325`),
  `destroy_dev()` (`:279`), and `devvn_refthread()`/`dev_relthread()`
  (`:285-286`) for per-open instance references;
- ioctl encoding via `<sys/ioccom.h>`;
- **every** command validates version, structure size, node ids, interface
  names, cycle times and buffer sizes, and no upstream openPOWERLINK structure
  is exposed verbatim.

The event path keeps upstream's discipline: the kernel event thread drains
`kEventQueueKInt` and `kEventQueueU2K` only, and never `K2U`, so the RT side
never waits for userland (`event/eventkcal-linuxkernel.c:544-555`). The FreeBSD
CAL must preserve that separation.

---

## 8. Callback contexts and the RT contract

From the study, with upstream's own comments:

| Callback | Context | May sleep | May allocate |
|---|---|---|---|
| EDRV RX → `dllkframe_processFrameReceived` | interrupt | no | no (none upstream) |
| EDRV cyclic error → `dllk_cbCyclicError` | interrupt | no | no |
| EDRV cycle sync → `cbMnSyncHandler` | interrupt | no | no |
| `cbCnTimerSync`, `cbCnLossOfSync`, `cbCnPresFallbackTimeout` | timer | no | no |
| DLL async / RPDO handlers | interrupt | no | no |
| DLL sync / TPDO handlers | event | yes | no |
| NMT event sink, error handler | both | must be context agnostic | no |

Two upstream constraints must survive the port:

- `cbMnSyncHandler` states at `dllknode.c:1311` that the cycle-finish work
  *"has to be done inside the callback function triggered by interrupt"*, so
  the RT thread must provide the same re-entrancy guarantee as the EDRV timer
  callback and keep `eventk_postEvent(kEventTypeDllkCycleFinish)` deferred.
- `cbCnTimerSync` posts `kEventTypeSync` from the timer because cycle
  preparation must complete before the SoC deadline.

**Allocation.** Every allocation in the RX, cycle, RT and event paths is absent
upstream: the RX path parses, posts events and writes pre-mapped shared memory;
the cycle path only submits pre-allocated TX buffers; the event path only drains
pre-built queues. All allocation is initialisation, configuration or teardown
(`dllknode.c:1264`, `dllkframe.c:865`, `pdok.c:261,289`). The port must
preserve this, which is why the backend and buffers are preallocated at
configuration time and reconfiguration follows stop/reconfigure/restart (§34).

---

## 9. Defects that must be fixed rather than reused

The study found six places where upstream indexes an array with a value derived
from a received frame. These are panic candidates in a kernel and are treated
as mandatory fixes, not optional hardening:

1. `dllkframe.c:1886` — `&aPresForward[nodeId - 1]` where `nodeId` is an
   unvalidated `UINT8`; node id 0 underflows to index 255. Read at `:1887`, and
   **written** at `:1909`. Note `dllknode_getNodeInfo()` (`:1931`) is correctly
   range-checked; this path bypasses it.
2. `dllkframe.c:836` — dereferences `pTxBuffer` one past the end of
   `pTxBuffer[]` when every PReq slot is occupied.
3. `dllkframe.c:448` — indexes a two-element array from a pointer subtraction
   on the TX-done callback argument, with no clamp.
4. `dllkframe.c:2898-2927` and `dllknode.c:1312-1321` — walk
   `aCnNodeIdList` to a sentinel with no length bound, on a list filled
   unbounded at `dllknode.c:836-867`.
5. `dllkframe.c:3141` — filter array indexed by caller constant with no internal
   check (safe by convention only).
6. `dllkframe.c:2598-2599` — wrap-indexed arrays relying on an invariant
   enforced elsewhere.

Correctly guarded, for contrast: `dllk.c:468`, `dllknode.c:942`,
`dllkframe.c:2644,1772,1833,682`. The port keeps those.

### Changes applied

Findings 1 to 5 have been applied in the reference tree at `/tmp/kilo/oplk`
(temporary; **not** vendored into the FreeBSD tree). Each is a local guard that
leaves upstream behaviour unchanged for well-formed input:

1. `processReceivedPres()` now rejects node id 0 and any id above
   `tabentries(aPresForward)` before using it as an index, and skips the
   forwarding block when the id is out of range.
2. `dllkframe_createTxFrame()` searches for a free PReq slot without advancing
   a pointer past the end of the array, and reports
   `kErrorEdrvNoFreeBufEntry` when the search finds nothing, instead of
   dereferencing one past the end.
3. `dllkframe_processTransmittedNonPlk()` only updates
   `aTxBufferStateNonPlk[]` when the EDRV returned one of the two
   non-POWERLINK TX buffers, since the offset is used as an index into a two
   element array.
4. The CN node-id list is bounded on all three sides: the fill in
   `dllknode_setupSyncPhase()` stops at the end of the list and only writes the
   PRC sentinel when it was not truncated, the sentinel write itself is bounds
   checked, and both readers — `searchNodeInfo()` and `cbMnSyncHandler()` — stop
   at the terminator or the end of the list.
5. `enableRxFilter()` rejects a filter entry at or beyond
   `tabentries(aFilter)`. Every current caller passes a constant in range, so
   this is defence in depth rather than a live bug.

Finding 6 was **not** a defect: `curLastSoaReq` is wrapped at
`dllkstatemachine.c:425-427`, so `aLastReqServiceId[]` and
`aLastTargetNodeId[]` are always indexed in range. It is recorded here only so
it is not re-investigated.

**These changes are not compile verified.** No `cmake` on this host, so the
upstream userspace library cannot be built here, and the fixes must be compiled
as part of the port. They are also deliberately *not* carried as patch files.

---

## 10. Locking map

The upstream DLL critical section exists only on Linux, as one file-static
spinlock, and compiles to nothing everywhere else
(`include/kernel/dllktgt.h:46-69`). FreeBSD needs the real thing, because the
cycle/RT thread, the event thread, the netgraph node and the EDRV callbacks are
genuinely concurrent — closer to the Linux case than to the no-op case.

| Object | Lock | Context | Notes |
|---|---|---|---|
| DLL instance | `mtx`, recreated per `_KERNEL` branch of `dllktgt.h` | all | replaces the spinlock; never held from the cycle path |
| TX list / cyclic EDRV instance | none upstream; FreeBSD holds them single-writer by construction | RT | one writer: the RT thread |
| RX buffers | ownership by EDRV, deferred release flag | EDRV RX | no lock; the buffer is owned by whoever holds it |
| netgraph hooks | framework | netgraph | netgraph already serialises per node |
| configuration | `mtx` | control | never taken from the RT path |
| statistics | `counter_u64` | any | lock-free per-CPU add on amd64 (`sys/amd64/include/counter.h:87-93`) |

Lock ordering: configuration before the DLL instance; the RT path takes no
lock. No inversion is possible because the RT path acquires nothing.

---

## 11. Module lifecycle

`SI_SUB_PSEUDO` is the right ordering slot — the same one `if_bridge`,
`if_lagg`, `if_tuntap` and `ng_modbus` use (`sys/sys/kernel.h:156`); the
netgraph base is `SI_SUB_NETGRAPH` (`:144`) and ordering must increase
(`:93`).

Unload refusal is `EBUSY` from `MOD_UNLOAD`, the idiom netgraph itself uses
(`ng_base.c:3240-3243`), with `kld_unload_try` available if refusal must happen
earlier (`sys/sys/eventhandler.h:280-283`, invoked at
`sys/kern/kern_linker.c:744`).

Initialisation order follows upstream's dependency chain: target, timer, event,
error handler, EDRV, cyclic EDRV, DLL, NMT, PDO, control, netgraph
registration. Teardown is the exact reverse, and drains callouts with
`callout_drain()` before freeing anything.

---

## 12. Statistics

Per-instance counters go through the netgraph control messages, as the survey
confirms is still the netgraph way (`ng_bridge.c:282-302`, `:815-850`), backed
by `counter_u64` so the RT path can update them without a lock. For anything
outside netgraph, the ipfilter `sysctl_ctx_list` pattern is the tree's own
precedent, including its `ENOTEMPTY` refusal-to-unload behaviour
(`sys/netpfil/ipfilter/netinet/mlfk_ipl.c:612-653`).

---

## 13. What to build, in what order

Priority 1, no protocol code:
`powerlink.ko` skeleton, `ng_powerlink` skeleton, `struct powerlink_instance`,
the FreeBSD target layer, `/dev/powerlink` via `make_dev()`, the netgraph node
with `lower` and its control messages, unload refusal.

Priority 2: the EDRV interface as written, the netgraph virtual EDRV, TX/RX
buffer management, filters.

Priority 3: `hrestimer` FreeBSD backend, `edrvcyclic.c`, `powerlink_rt.c`.

Priority 4: DLL, NMT, PDO, event and control CALs.

Priority 5: virtual MN and CN over netgraph.

Priority 6 onward: hardware EDRV, timing work, launch-time TX.

---

## 13a. Priority 1 status: skeleton in place

Implemented and compile verified:

```
sys/net/powerlink/powerlink.h            limits, runtime configuration, roles
sys/net/powerlink/powerlink_edrv.h       the EDRV contract, from edrv.h
sys/net/powerlink/powerlink_edrv_freebsd.h  FreeBSD-only extensions
sys/net/powerlink/powerlink_core.h/.c   the instance, counters, state
sys/net/powerlink/powerlink_edrv_ng.h/.c the netgraph virtual driver
sys/net/powerlink/powerlink_rt.h/.c      the real time thread
sys/net/powerlink/powerlink_timer.h/.c   deadline timers over callouts
sys/netgraph/ng_powerlink.h/.c           the node and its control messages
sys/modules/powerlink/Makefile           builds powerlink.ko
```

The five objects build with zero errors and zero warnings under `-Werror`,
and every file is clean under `checkstyle9.pl`.

What is deliberately not there yet: the protocol engine, so the cycle callback
counts nothing and the receive path releases frames it has not yet dispatched.
The wiring above them is complete, which is the point of doing the skeleton
first.  The instance is created by the node constructor and refused with EBUSY
for a second node, the module refuses to unload while an instance exists, and
the counters are `counter(9)` counters so the data path needs no lock.

Five API mistakes were caught by compiling rather than by reading, and are
worth recording because the tree differs from what the header names suggest:

  * there is no `thread_join()` or `thread_cancel()` a module may use, so the
    cycle thread stops cooperatively and announces it through a condition
    variable with a bounded wait;
  * there is no kernel `nanosleep()`; the thread sleeps with `pause_sbt()`,
    which is what makes a sub-tick deadline expressible at all;
  * `kthread_exit()` takes no argument, and the thread is created with
    `kthread_add()`, not `kthread_create()`;
  * `mtx_init()` is a macro taking a name string and option flags, not a type
    constant, so the call reads `mtx_init(&rt->lock, "pl_rt", NULL, MTX_SPIN)`;
  * `counter_u64_t` is a pointer to a per-CPU counter, so the counters are
    allocated when the instance is created and freed when it goes away, and
    the structure reported to the control plane holds values, not pointers.

Also worth recording: this workspace copy is missing `sys/mtx.h` and
`sys/string.h`, so module builds here need local stand-ins outside the
repository.  Nothing in the port depends on them being absent.

## 14. Open questions

1. **Cycle time target.** Upstream's own timer floor is 100 µs and POWERLINK's
   practical range is hundreds of microseconds to milliseconds. Confirm the
   cycle time to design for; it decides whether the spin-to-deadline margin is
   small or most of the period.
2. **Single instance accepted?** The port will ship one instance. Confirm that
   is acceptable, or that multiple instances are required now, which changes
   the order of work substantially.
3. **Shared memory.** Upstream's PDO memory is `shm_open`/`mmap` shared between
   a kernel stack and a userland process. Confirm the FreeBSD PDO model: a
   kernel-only stack with a netgraph virtual EDRV needs none, but a
   userland POWERLINK application does.
4. **`powerlinkctl` scope.** The control utility is specified in §44 but is not
   required for the protocol milestone. Confirm whether to build it with the
   KLD or later.
5. **OD configuration.** Upstream compiles ODC data into the stack
   (`CONFIG_OBD_DEF_CONCISEDCF_FILENAME`). Confirm whether the FreeBSD port
   needs live object dictionary access at all for the first milestone.

---

## 15. Verification performed

- Upstream cloned at the tag and read directly; the authoritative build source
  list was taken from `stack/cmake/stackfiles.cmake` rather than from directory
  listings, so the inventory matches what upstream actually compiles.
- 807 functions extracted mechanically (`ng_powerlink/gen_porting_table.py`).
- Four independent studies: the EDRV and cyclic-EDRV contract; the protocol
  core with a global-state audit; the platform CAL layers; and the FreeBSD
  facilities, with each claim carrying a `file:line`.
- Two claims I made before the survey were wrong and are corrected here: that
  callouts cannot deliver sub-millisecond periods (they can, without
  `C_HARDCLOCK`), and that FreeBSD exposes `kern.rtprio.*` tunables and
  `uio_set_cpuset_seq()` (neither exists in this tree).
- Nothing has been compiled or run: no FreeBSD host yet, and no code written.