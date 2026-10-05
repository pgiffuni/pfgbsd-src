# ng_modbus — Phase 1 Research Report

Status: research complete, **no implementation code written**.
Tree: FreeBSD 16.0-CURRENT, branch `ng_modbus`, root `/home/pedro/projects/pfgbsd`.
References are `file:line` in that tree unless stated otherwise.

Deliverable for §62 Phase 1 / §64 of the task statement. Nothing is committed.

---

## 0. Executive summary of what matters

1. **This tree uses the FreeBSD-native (struct `ng_type`) netgraph API**, not the
   OpenBSD-style `ng_register_node_type()`/`NGNODE_DECLARE` API. Every design
   point below is written against what actually exists here.
2. **The netgraph framework already serializes per-node callbacks** with a
   multiple-reader/single-writer gate. With `NG_NODE_FORCE_WRITER`, *all*
   callbacks become exclusive, so the node needs **no lock of its own** for the
   common case (§14).
3. **`ng_socket` cannot carry a byte stream** — both its `protosw` entries are
   `SOCK_DGRAM` (`sys/netgraph/ng_socket.c:1054-1075`). **§24/§41 resolve to
   `ng_ksocket`**, which is stream-capable (`sys/netgraph/ng_ksocket.c:1194-1249`).
4. **FreeBSD has no RS-485 support** in `sys/dev/serial` (no `TIOCSRS485`), and
   `ng_tty(4)` gives bytes but no timing. RTU framing therefore lives in
   `ng_modbus`, using `callout_reset_sbt()` (`sys/sys/callout.h:97-108`) for
   sub-millisecond t1.5/t3.5 gaps (§12, §15).
5. **This workspace is a pruned FreeBSD tree** (no `sys/kern/Makefile`,
   `sys/kern/kern.mk`, `sys/kern/makesys.mk`, `share/doc/style`, `tools/style`,
   `sys/amd64/include/machine/`). A working **BSD make module build with FreeBSD's
   real `-Werror` flag set was validated** (§3.4); linking to `.ko` needs `lld`,
   and `kldload`/`ngctl` testing needs a FreeBSD runtime that this host does not
   have.
6. `kldload`-level acceptance (§44 Milestone 1) **cannot be executed on this
   host**. See §18.4 and §20 Q1/Q2 for the options that unblock it.

---

## 1. FreeBSD netgraph nodes examined

All under `sys/netgraph/`. Nothing was copied; patterns only.

| Node | Why examined | Key structural takeaway |
|---|---|---|
| `ng_sample.c` (template) | canonical skeleton | cmdlist → `struct ng_type` → `NETGRAPH_INIT()`; ctor/newhook/rcvmsg/rcvdata/shutdown/connect/disconnect (`ng_sample.c:86-118`, `:148-484`) |
| `ng_echo.c` | smallest complete node | minimal ctor, `NG_FWD_ITEM_HOOK`, self-destruct on last hook disconnect (`ng_echo.c:74-118`) |
| `ng_split.c` | cleanest small node, modern designated initializers | hook private slot stored as `hook_p *`; `NG_NODE_SET_PRIVATE(node, NULL)` in shutdown (`ng_split.c:81-176`) |
| `ng_hub.c` | fan-out | `NG_NODE_FOREACH_HOOK` iteration (`:129-143`) |
| `ng_tee.c` | multiple hooks + per-hook counters | per-hook stats struct in node private (`:70-74`, `:303-330`) |
| `ng_pipe.c` | **queue + callout pattern** | `ng_callout_init()` in ctor (`:261`), `ng_callout()` to re-arm (`:895-899`), `ng_uncallout()` in shutdown (`:933-934`), per-hook mbuf queues drained in disconnect (`:962-978`) |
| `ng_deflate.c` | stateful stream transform | `NG_NODE_FORCE_WRITER` (`:180`), fixed in/out buffers, `getstats`/`clrstats` triple (`:119-139`, `:288-305`) |
| `ng_socket.c` | control/data node + domain | message plumbing, `MODULE_DEPEND` idiom, `ng_mod_event` (`ng_socket.c:812-1115`) |
| `ng_ksocket.c` | **TCP stream node** | STREAM header-mbuf trick, EOF as zero-length mbuf, accept children (`ng_ksocket.c:968-1328`) |
| `ng_tty.c` | **serial byte source** | `ttyhook_register()` on a pid/fd (`:257-271`), single `"hook"` name, hotchar flush, `ifqueue` output (`:308-399`) |
| `ng_ether.c` | mbuf-heavy, many control messages | `arglen` validation + `error = EINVAL` pattern (`:496-650`), `NG_HOOK_SET_TO_INBOUND` + `NG_OUTBOUND_THREAD_REF()` loop guard (`:287-289`) |
| `ng_nat.c` | many commands, variable-size replies | `NG_MKRESPONSE()` with computed size (`:661-662`) |

Framework: `netgraph.h` (types/macros), `ng_base.c` (scheduler, node/hook
lifetime, module events), `ng_parse.[ch]` (text control-message marshalling),
`ng_message.h` (NGM_* cookies/commands), `NOTES` (authoritative usage notes —
`NOTES:76-79` mbuf invariant, `NOTES:83-90` feedback loops).

---

## 2. Source map (files that matter)

```
sys/netgraph/netgraph.h      node/hook/type decls, macros, NG_* send macros
sys/netgraph/ng_base.c        ng_newtype/ng_rmtype/ng_rmnode/ng_make_node_common,
                              ng_add_hook/ng_destroy_hook, ng_snd_item scheduler,
                              ng_callout/ng_uncallout, ng_mod_event, ngthread
sys/netgraph/ng_message.h     NGM_GENERIC_COOKIE, NGM_* commands, NG_MKRESPONSE
sys/netgraph/ng_parse.[ch]    ng_parse_type tables used by ngctl text messages
sys/netgraph/NOTES            framework usage contract
sys/kern/kern_linker.c        unload sequence (module_quiesce/unload)
sys/kern/kern_module.c        MOD_LOAD/UNLOAD/QUIESCE dispatch
sys/sys/callout.h             callout_init/reset/reset_sbt
sys/sys/time.h                SBT time base, ustosbt()/sbttous()
sys/sys/endian.h              be16dec/be16enc/be32dec/be32enc (:73-147)
sys/sys/mbuf.h                m_length/m_pullup/m_append/m_copydata/m_getm2
sys/sys/param.h               MSIZE 256 (:192), MCLSHIFT/MCLBYTES 2048 (:196-199)
sys/conf/kmod.mk              KLD build machinery (:92 WERROR, :119-131 flags,
                              :409-426 opt_*.h, :588 include kern.mk)
sys/conf/kern.mk              CWARNFLAGS (:5-10)
share/mk/bsd.kmod.mk          thin shim -> sys/conf/kmod.mk
sys/modules/Makefile          SUBDIR list (:14.., sorted at :994)
sys/modules/netgraph/         per-node KLD dirs + Makefile.inc (.PATH sys/netgraph)
tests/sys/netgraph/           kyua/ATF netgraph test suite + libnetgraph harness
tools/build/checkstyle9.pl    style(9) mechanical checker
tools/build/make.py           cross-build bootstrap used by this repo's CI
```

---

## 3. KLD / module conventions

### 3.1 Registration (netgraph-specific)

`NETGRAPH_INIT(tn, tp)` (`netgraph.h:1110-1122`) expands to `moduledata_t
ng_<tn>_mod = { "ng_<tn>", ng_mod_event, tp }` + `DECLARE_MODULE()` +
`MODULE_DEPEND(ng_<tn>, netgraph, NG_ABI_VERSION ×3)`. The framework calls
`ng_newtype()` on MOD_LOAD (`ng_base.c:3090-3118`); `ng_mod_event` refuses
unload with `EBUSY` while `type->refs > 1`, i.e. while any node instance lives
(`ng_base.c:3113-3115`). Duplicate type name → `EEXIST` (`ng_base.c:1288-1291`);
ABI/name validation → `EINVAL` (`ng_base.c:1277-1284`).

So `modbus.ko` needs **no hand-written module init/exit** unless it needs extra
resources; `NETGRAPH_INIT(modbus, &typestruct)` registers `ng_modbus`, and
`ngctl`'s `mkpeer modbus:` will even auto-`kldload` it (`ng_socket.c:251-282`).

### 3.2 Build wiring

A self-contained module needs exactly **one** change: an entry in
`sys/modules/Makefile`'s `SUBDIR` (alphabetical; the list is re-sorted at
`:994`). No `sys/conf/files` / `ObsoleteFiles.inc` change is required — those
are for code that must also compile into the kernel image via an `optional
<pseudo-device>` entry (`sys/conf/files:4392-4431`).

Proposed (per §8 of the task):

```make
# sys/modules/modbus/Makefile
.PATH:	${SRCTOP}/sys/netgraph
.PATH:	${SRCTOP}/sys/net/modbus

KMOD=	modbus
SRCS=	ng_modbus.c			\
	modbus.c				\
	modbus_pdu.c				\
	...
.include <bsd.kmod.mk>
```

Header style for module dirs (representative): `sys/modules/aout/Makefile:1-7`,
`sys/modules/if_epair/Makefile:1-6`, `sys/modules/netgraph/deflate/Makefile:1-4`.
Private headers go in `SRCS` (there is no `MYHDRS` in this tree;
`OBJS+= ${SRCS:N*.h:R:S/$/.o/g}`, `sys/conf/kmod.mk:227`).

### 3.3 Warning flags

`CFLAGS+= -Wall` is *not* the convention. Warnings come from `CWARNFLAGS`
(`sys/conf/kern.mk:5-10`): `-Wall -Wstrict-prototypes -Wmissing-prototypes
-Wpointer-arith -Wcast-qual -Wundef …`, and `sys/conf/kmod.mk:92` sets
`WERROR?=-Werror`. Building the module therefore *is* the "compile with warnings
enabled, eliminate new warnings" gate required by §3.

### 3.4 Build verification harness (validated on this host)

This workspace is pruned (`sys/kern/Makefile`, `sys/kern/kern.mk`,
`sys/kern/makesys.mk`, `sys/amd64/include/machine/` absent), so a kernel build
is impossible. A **module build works**, because `sys/conf/kmod.mk` +
`sys/conf/kern.mk` are intact and the build itself creates the `machine`/`x86`
include symlinks. Working invocation (proven by compiling `ng_echo.ko` objects
with zero errors):

```sh
mkdir -p /tmp/kbdir                 # stand-in for KERNBUILDDIR
ln -s $PWD/sys/amd64/include /tmp/kbdir/machine
ln -s $PWD/sys/x86/include   /tmp/kbdir/x86
for h in opt_global opt_netgraph opt_kdb opt_platform opt_inet opt_inet6; do
        : > /tmp/kbdir/$h.h
done

cd sys/modules/modbus
make -m $PWD/share/mk \
      MAKEOBJDIRPREFIX=/tmp/obj KERNBUILDDIR=/tmp/kbdir \
      MACHINE=amd64 MACHINE_CPUARCH=amd64 \
      SYSDIR=$PWD/sys SRCDIR=$PWD/sys KMODDIR=/tmp/modules \
      CC="clang --target=x86_64-unknown-freebsd16.0" \
      LD="ld.lld --target=x86_64-unknown-freebsd16.0"
```

Compile works today. **Link does not**: this host has no `lld` (GNU
`ld.bfd` rejects `elf_x86_64_fbsd`). `kyua` is installed but is useless without
a FreeBSD system to run it on.

### 3.5 Licensing headers

Current FreeBSD practice = SPDX 2-clause block. Copy this shape (verbatim from
`sys/net/if_geneve.c:1-27`, same wording at `sys/netgraph/ng_ipfw.c:1-26`,
`ng_pred1.c:1-26`, `sys/net/dummymbuf.c:1-26`):

```c
/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 <copyright holder>
 * All rights reserved.
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
```

Do **not** copy the legacy Whistle headers from `ng_socket.c:5-40` or
`ng_ether.c:6-40`. The task's 2-clause requirement matches this exactly.

---

## 4. style(9) — what is actually enforced

Authoritative text (`share/doc/style/9.style`) is **absent from this tree**;
`CONTRIBUTING.md:153` points to the published style(9) man page. Mechanical
enforcement is `tools/build/checkstyle9.pl` (v0.31, `:17`), wired in CI as
`.github/workflows/style.yml`.

```
perl tools/build/checkstyle9.pl -f sys/netgraph/ng_modbus.c sys/netgraph/ng_modbus.h
perl tools/build/checkstyle9.pl --branch <base>..HEAD
```

Rules that will bite, with line numbers in the checker:

- line length: **>80 = warning, >120 = error** (`:1522-1533`); exempt: sole-URL
  lines, bare string-literal lines, everything under `tests/`
- trailing whitespace (`:1512-1516`), missing newline at EOF (`:1541-1543`),
  DOS line endings (`:1498-1500`), invalid/doubly-encoded UTF-8 (`:1458-1471`)
- block comments: `/*` alone on its line (`:1556`), `*` on continuation lines
  (`:1562`), `*/` alone on its line (`:1571`), aligned `*` (`:1579`)
- declarations: storage class first (`:2549`), `inline` between storage class
  and type (`:2555`), **no `extern` declarations inside `.c` files** (`:2566-2589`),
  function-declaration arguments on the same line as the identifier (`:2582`)
- braces: opening `{` of `if/while/for/switch/else` on the previous line
  (`:1718`), function brace on the next line (`:1909`), space before `{` after
  `)` (`:2148`), `else` after `}` at the same indent (`:2284`), **`switch` and
  its `case`/`default` at the same indent** (`:1671-1689`, FreeBSD-specific)
- statements after `if/while/for/else/case` go on the next line (`:2213-2280`)
- operator spacing table (`:1970-2142`): `->` no spaces, space after `;` and
  `,`, unary leading space only, no space between function name and `(`
  (`:1936-1967`), space required after `if/while/for/switch/return` before `(`
  (`:2189`)
- body indent must be a multiple of 4 and greater than the condition's (`:1826`)
- multi-statement macros must be `do { } while (0)` (`:2311-2389`); complex
  macro values parenthesized (`:2390`)
- `__FUNCTION__` → `__func__` (`:2593`), `%Ld/%Lu` → `%lld/%llu` (`:2604`),
  `signal(3)` for handlers banned (`:2598`), `sizeof(&x)` discouraged (`:2561`)

Tabs vs spaces is **not** checked mechanically; `.editorconfig` and
`.clang-format` at the tree root carry that policy. SPDX/copyright blocks are
**not** checked at all — they are a review item.

---

## 5. Modbus Application Protocol V1.1b3 — protocol map

Source: `MODBUS Application Protocol Specification V1.1b3`, Modbus Organization,
26 Apr 2012. Retrieved from `https://www.modbus.org/file/secure/modbusprotocolspecification.pdf`
(official distribution; the `modbus.org/docs/...` paths 404 behind the site's
WAF). Text extracted locally for reference at
`/tmp/kilo/modbus-specs/app_protocol_v1_1b3.para.txt`. The spec is used only as
the technical definition of the protocol; **no code is derived from any
implementation**, and none of this text will be copied into source comments
beyond short section references.

| Requirement | Spec section | Value / rule |
|---|---|---|
| PDU composition | 4.1 | `Function code (1B) + Data (0..252B)` |
| Function code range | 4.1 | valid 1..255; **FC 0 invalid**; 128..255 reserved for exception responses (response FC = request FC \| 0x80) |
| Data encoding | 4.2 | **MSB first** for every multi-byte quantity |
| ADU, RTU | 4.1 | `Address(1) + PDU(≤253) + CRC(2)` = **max 256 bytes** |
| ADU, TCP | 4.1 | `MBAP(7) + PDU(≤253)` = **max 260 bytes** |
| ADU, ASCII | Serial §2.5.2.1 | **max 513 characters** |
| Addressing model | 4.4 | data numbered X is addressed as X−1; PDU addressing is 0-based; wrap at 65535→0 |
| Data model | 4.3 | four areas: discrete inputs (RO), coils (RW), input registers (RO), holding registers (RW), each 16-bit-addressable up to 65536 items |
| Transaction | 4.5 | client/server request-response; server may not initiate |
| Exception responses | 7 | see table below |
| Reserved codes | Annex A | 8/19, 8/21-65535, 9, 10, 13, 14, 41, 42, 90, 91, 125, 126, 127 |

### Exception codes (§7)

| Code | Name | Use in this implementation |
|---|---|---|
| 01 | ILLEGAL FUNCTION | FC not implemented by this node |
| 02 | ILLEGAL DATA ADDRESS | address+quantity outside backend map (or unimplemented area) |
| 03 | ILLEGAL DATA VALUE | inconsistent length/byte-count, bad quantity range, malformed field |
| 04 | SERVER DEVICE FAILURE | backend returned an unrecoverable error |
| 05 | ACKNOWLEDGE | programming commands only — **not generated in v1** |
| 06 | SERVER DEVICE BUSY | programming commands only — **not generated in v1** |
| 07 | NEGATIVE ACKNOWLEDGE | programming commands only — **not generated in v1** |
| 08 | MEMORY PARITY ERROR | FC 20/21 only — **not generated in v1** |
| 09 | GATEWAY TARGET DEVICE FAILED TO RESPOND | gateway role only |
| 0A | GATEWAY PATH UNAVAILABLE | gateway role only |

§15/§50 requirement: these are a **separate enum** from errno. Only 01/02/03/04
are reachable in the first implementation; the enum reserves the rest so future
code cannot reuse them.

---

## 6. Modbus over Serial Line V1.02 — protocol map

Source: `MODBUS over Serial Line Specification and Implementation Guide V1.02`,
Dec 20 2006, from `https://www.modbus.org/file/secure/modbusoverserial.pdf`.
Local extract: `/tmp/kilo/modbus-specs/serial_line_v1_02.para.txt`.

| Requirement | Spec section | Value / rule |
|---|---|---|
| Bus model | 2.1 | single master, 1..247 slaves, **one transaction at a time**; slaves never transmit unsolicited |
| Addressing | 2.2 | 0 = broadcast, 1..247 = slave addresses, 248..255 reserved |
| Broadcast | 2.1 | write functions only; **no response is ever sent** |
| RTU frame | 2.5.1.1 | `Slave address(1) + FC(1) + Data(0..252) + CRC(2)`; max 256 B |
| Frame delimiter | 2.5.1.1 | **t3.5 = 3.5 character times** of silence |
| Inter-character | 2.5.1.1 | **t1.5 = 1.5 character times**; longer ⇒ frame incomplete, discard |
| Timing rule | 2.5.1.1 | ≤19200 Bd: compute from character time; **>19200 Bd: use fixed t1.5 = 750 µs, t3.5 = 1.750 ms** |
| CRC | 2.5.1.2, App. B | 16-bit, init `0xFFFF`, poly **`0xA001`** (reversed 0x8005), LSB-first per byte, **low-order byte transmitted first** |
| ASCII framing | 2.5.2.1 | `':'` + address/data as 2 hex chars each + LRC + `CR` `LF`; only hex chars `0-9A-F` allowed elsewhere; inter-character gap up to 1 s permitted |
| LRC | 2.5.2.2, App. B | 8-bit: sum all bytes (carries discarded), then **two's complement**; high-order hex char first |
| ASCII example | App. B | byte `0x5B` → `"5B"`; LRC `0x61` → `"61"`; CRC `0x1241` → wire `0x41 0x12` |

CRC test vector straight from Appendix B: frame `02 07` → CRC register
`0x1241`, transmitted **lo `0x41` first, then hi `0x12`**. This is the primary
known-answer test for `modbus_crc.c` (§28).

---

## 7. Function-code inventory (V1.1b3 §6)

| § | FC | Name | Req len (excl FC) | Limits | v1 scope |
|---|---|---|---|---|---|
| 6.1 | 01 | Read Coils | 4 | 1..2000 (0x7D0) coils | **implement** |
| 6.2 | 02 | Read Discrete Inputs | 4 | 1..2000 (0x7D0) | **implement** |
| 6.3 | 03 | Read Holding Registers | 4 | 1..125 (0x7D) | **implement** |
| 6.4 | 04 | Read Input Registers | 4 | 1..125 (0x7D) | **implement** |
| 6.5 | 05 | Write Single Coil | 4 | value ∈ {0x0000, 0xFF00} | **implement** |
| 6.6 | 06 | Write Single Register | 4 | any 16-bit | **implement** |
| 6.7 | 07 | Read Exception Status | 0 | serial-line only, 7-bit byte | defer |
| 6.8 | 08 | Diagnostics | 4 | 21 sub-functions | defer |
| 6.9 | 11 (0x0B) | Get Comm Event Counter | 0 | serial-line only | defer |
| 6.10 | 12 (0x0C) | Get Comm Event Log | 0 | serial-line only | defer |
| 6.11 | 15 (0x0F) | Write Multiple Coils | 5+N/8 | 1..2000 (0x7D0); byte count must equal ceil(N/8) | **implement** |
| 6.12 | 16 (0x10) | Write Multiple Registers | 5+2N | 1..123 (0x7B) | **implement** |
| 6.13 | 17 (0x11) | Report Server ID | 0 | serial-line only | defer |
| 6.14 | 20 (0x14) | Read File Record | mixed | reference types 1-6 | defer |
| 6.15 | 21 (0x15) | Write File Record | mixed | reference types 1-6 | defer |
| 6.16 | 22 (0x16) | Mask Write Register | 6 | `Result = (Cur & And) \| (Or & ~And)`; request echoed in response | **implement** |
| 6.17 | 23 (0x17) | Read/Write Multiple Registers | 9+2W | write first, then read; read ≤125, write ≤121 (0x79) | **implement** |
| 6.18 | 24 (0x18) | Read FIFO Queue | 2 | ≤32 registers, else ex. 03 | defer |
| 6.19 | 43 (0x2B) | Encapsulated Interface Transport | variable | MEI 0x01-0x0F | defer |
| 6.20 | 43/13 | CANopen General Reference | — | not a Modbus data model | never |
| 6.21 | 43/14 | Read Device Identification | 4 | object dictionary read-only | defer (nice for FC 0x11 substitute) |

Rationale for the v1 set: the 11 codes in §16 of the task plus nothing else.
They cover the entire public data model, both write styles, mask write, and the
combined read/write, and every one of them has unambiguous validation rules.

---

## 8. Proposed node / hook architecture

### 8.1 Node

One node type, `ng_modbus`, registered by
`NETGRAPH_INIT(modbus, &ng_modbus_typestruct)`; KLD name `modbus`, type name
`modbus` (`NG_TYPESIZ` 32). Private state in `struct ng_modbus_private`
allocated in the constructor, `NG_NODE_SET_PRIVATE()`ed, with `priv->node`
back-pointer.

### 8.2 Hooks

Start with **one data hook** plus node-level control messages:

```
ng_modbus
   |
 "lower"    ← transport bytes/frames (peer varies by transport)
 (control messages arrive on the node or on "lower"; netgraph delivers rcvmsg
  through the named hook or the node — ng_base.c:2443-2450)
```

Rationale against §10: netgraph control messages are already addressed to a
node, and adding a `control` hook would only add a hook with no data
traffic; existing nodes (e.g. `ng_socket`) receive control messages on the data
hooks. Later transports are a **node attribute**, not extra hooks
(`NGM_MODBUS_SET_TRANSPORT`), which keeps hook names stable and keeps the
transport layer out of the graph shape. If a second hook is ever needed, the
in-tree precedent for per-hook addresses is `ng_hole.c:160-169`
(`getstats <hookname>` via `ng_parse_hookbuf_type`).

### 8.3 Roles

`role ∈ {SERVER, CLIENT}` — server is the default (a device). Client mode is
one outstanding transaction at a time (§20).

---

## 9. Proposed internal PDU representation

Fixed-size, transport-neutral, no pointers into wire buffers past validation:

```c
#define	MODBUS_FC_MIN		1
#define	MODBUS_FC_EXCEPTION	0x80	/* response flag */
#define	MODBUS_PDU_MAXLEN	253	/* serial-line derived (App. §4.1) */
#define	MODBUS_DATA_MAXLEN	252

struct modbus_pdu {			/* borrowed, validated view */
	uint8_t		 fc;		/* 1..127 after validation */
	uint16_t		 len;		/* data length, 0..252 */
	const uint8_t	*data;
};

struct modbus_pdu_buf {			/* owned, encode target */
	uint8_t		 buf[MODBUS_PDU_MAXLEN];
	uint16_t		 len;		/* bytes used, FC included */
};
```

Rules:
- `modbus_pdu_parse()` validates `1 ≤ len ≤ 253`, `fc != 0`; the raw slice is
  never exposed unvalidated (§14).
- Handlers receive `struct modbus_pdu` and write into `struct modbus_pdu_buf`.
  They never see MBAP, CRC, LRC, ASCII, or mbufs (§11).
- Response/exception is expressed by the builder, not by special-casing:
  `modbus_pdu_exception(&out, fc, modbus_exc_t)`.
- Byte access is always explicit: `be16dec()`/`be16enc()` (`sys/sys/endian.h:73-147`).
  No casts of packet bytes to structs (§13).

---

## 10. Proposed backend API

```c
typedef enum {
	MODBUS_AREA_COIL,		/* 01, 05, 15   */
	MODBUS_AREA_DISCRETE_IN,	/* 02           */
	MODBUS_AREA_INPUT_REG,		/* 04           */
	MODBUS_AREA_HOLDING_REG		/* 03, 06, 16, 22, 23 */
} modbus_area_t;

struct modbus_backend {
	void	*ctx;
	/* Return MODBUS_OK, or a modbus_exc_t (2 = address, 4 = device failure). */
	int	(*read_bits)(void *, modbus_area_t, uint16_t addr,
		    uint16_t nbits, uint8_t *bits);
	int	(*write_bits)(void *, modbus_area_t, uint16_t addr,
		    uint16_t nbits, const uint8_t *bits);
	int	(*read_registers)(void *, modbus_area_t, uint16_t addr,
		    uint16_t nregs, uint16_t *regs);
	int	(*write_registers)(void *, modbus_area_t, uint16_t addr,
		    uint16_t nregs, const uint16_t *regs);
	int	(*mask_register)(void *, uint16_t addr,
		    uint16_t and_mask, uint16_t or_mask);
};
```

- Area is an enum, so an unimplemented area returns `MODBUS_EX_ILLEGAL_ADDRESS`
  (02) by construction rather than by accident.
- Backend returns **Modbus** status, not errno (§15/§50). Kernel errno appears
  only in netgraph control-message replies and in internal logs.
- First implementation: `modbus_mem.c`, one flat allocation per area, sizes
  fixed at configure time (control message), bounded by
  `NG_MODBUS_MAX_BACKEND_BYTES` (§39/§60). Registers held as host-order
  `uint16_t`; conversion happens in the PDU layer only.
- Size changes that allocate are privileged (§61): netgraph control messages
  already require `root` (socket `/dev/ngctl` is `0600 root`), which is the
  privilege model to reuse — no bespoke privilege system.

---

## 11. Proposed TCP architecture

**Answer to §24/§41: use `ng_ksocket`; do not write a TCP stack, and do not
use `ng_socket`.**

Evidence: `ng_socket` registers two `protosw` entries and **both are
`SOCK_DGRAM`** (`ng_socket.c:1054-1075`); it cannot carry a stream, and one
`write(2)` becomes exactly one netgraph item (`ngd_send`, `:342-422`), with no
reassembly. `ng_ksocket` supports `inet/stream/tcp`, explicitly notes that
"stream sockets do not have packet boundaries" (`ng_ksocket.c:1194-1214`), and
signals EOF with a zero-length mbuf guarded by `KSF_EOFSEEN`
(`:1241-1249`). Its accept path hands each connection to a child node
(`NGM_KSOCKET_ACCEPT`, `:1278-1328`).

Modbus-specific framing stays in `modbus_tcp.c`:

```
TCP byte stream (mbufs from ng_ksocket)
      │  mbap_parse(): state = {rxb, expect, tid, pid, unit, need}
      ▼
7-byte MBAP: TID(2) PID(2)==0 LEN(2) UID(1), then LEN-1 PDU bytes
      ▼
struct modbus_pdu  ──►  modbus engine  ──►  modbus_pdu_buf
      ▼
mbap_encode(): copy TID/PID verbatim, LEN = 1 + pdu.len
```

Stream-parser requirements (§22/§23), all testable without hardware:
- MBAP `LEN` is authoritative for the byte count and includes the unit id
  (TCP/IP Guide §3.1.3: "The length field is a byte count of the following
  fields, including the Unit Identifier and data fields").
- Validate `PID == 0`; validate `LEN` in `2..254` **before** buffering; reject
  `LEN > 1 + 253`.
- Never assume one mbuf == one ADU; handle split and coalesced ADUs, and
  partial headers.
- Bounded reassembly: a fixed `MODBUS_TCP_MAX_ADU` (260) byte window per
  connection, no dynamic growth.
- Server keeps one transaction id in flight per connection (request/response
  pairing, TCP/IP Guide §3.1.3); client allocates TID monotonically.

---

## 12. Proposed RTU architecture

```
ng_tty (bytes) or loopback/virtual transport
      ▼
modbus_rtu state machine: IDLE → RECEIVING → (t1.5 expiry ⇒ drop) → FRAME_READY
      ▼
modbus_crc_verify(addr|fc|data, 2-byte LE CRC)
      ▼
struct modbus_pdu
```

Timing model (§25/§26):
- `t1.5` and `t3.5` are computed from the configured baud and bits/char, with
  the spec's fixed values above 19200 Bd.
- **Implementation mechanism: `callout_reset_sbt(c, ustosbt(us), ...)`**
  (`sys/sys/callout.h:97-108`, `sys/sys/time.h:127-133,263-280`), *not*
  `ng_callout()`. `ng_callout()` is tick-granular (`netgraph.h:1167`), and at
  115200 Bd t3.5 ≈ 304 µs — below typical tick resolution. SBT gives
  nanosecond-valued deadlines.
- The SBT callback runs in callout softclk context, so it must touch **no
  node state**: it only calls `ng_send_fn()` (`netgraph.h:1159`,
  `ng_base.c:3716-3721`) to re-enter the node in writer context. This is what
  keeps the locking model in §14 lock-free (§15, §37).
- Real-time honesty (§26): Softclock scheduling jitter at typical load is tens
  of microseconds. RTU above 19200 Bd is therefore best-effort; the man page
  must say so, and the design must never depend on t3.5 being exact — a frame
  is also considered complete when its CRC arrives on a full buffer.
- Serial direction control (RS-485 DE/RE) is **not available**: no `TIOCSRS485`
  anywhere in `sys/`; only device-tree properties on some ARM SoCs. Physical
  half-duplex needs either an external transceiver with auto-direction or a new
  driver/ldisc decision — explicitly out of scope for v1 (§42).

Virtual RTU for tests: feed hand-built byte sequences into the same state
machine through the `lower` hook with an injected clock, so protocol tests need
no hardware (§58).

---

## 13. Proposed ASCII architecture

```
':'  hex pairs  LRC  CR LF        (modbus_ascii.c)
      ▼ decode hex, validate CR/LF
addr(1) + PDU(≤253) + LRC(1)     → same struct modbus_pdu
```

Rules (Serial §2.5.2.1/§2.5.2.2): frame starts at `':'`; a `':'` while a frame
is open discards the partial frame; only `0-9A-F` otherwise; LRC = two's
complement of the byte sum, transmitted high nibble first; max frame 513
characters; CR/LF validated as a pair. Implemented as a distinct state machine
sharing only the PDU layer with RTU (§29).

---

## 14. Locking model

**The framework already gives us this**, and it is the single most important
simplification available:

- Every node owns an input queue with an atomic MR/SW gate
  (`ng_base.c:1904-2172`). **No mutex is held while a node callback runs**
  (released at `ng_base.c:3440`).
- `rcvdata` runs as a **reader** (concurrent with other data items);
  `rcvmsg`, `newhook`, `connect`, `disconnect`, `shutdown`, `constructor` and
  all `ng_send_fn*`/callout functions run as **writer** (exclusive)
  (`ng_base.c:3542`, `:3564-3568`, `:3732`).
- Declaring `NG_NODE_FORCE_WRITER(node)` in the constructor upgrades data items
  to writers too (`ng_base.c:2264-2270`; used by `ng_deflate.c:180`,
  `ng_pipe.c:259`, `ng_nat.c:338`). **Recommendation: do this.** A Modbus node
  has real protocol state (parser, transaction slot, backend handles); exclusive
  delivery removes the need for the register/translator machinery the task
  anticipates in §38.
- Context rules (`ng_base.c:3428-3456`): data items are applied inside
  `NET_EPOCH_ENTER`; message/fn items outside the epoch.
  **`rcvdata` must not sleep and must allocate with `M_NOWAIT`** — but because
  items may also be delivered inline in the sender's context
  (`ng_base.c:2311-2323`), we keep `rcvdata` non-sleeping unconditionally.

Consequences for the design:
- Statistics counters: plain `uint64_t` `++` under exclusive delivery, no atomics
  needed (same as `ng_deflate.c:429-430`).
- Backend: no internal lock of its own; guarded by the node writer gate. The
  backend must never be touched from a callout context (§12).
- Rule to enforce in review: **no kernel mutex in `ng_modbus` at all**. If one
  ever becomes necessary it must be for state genuinely shared with a non-node
  context, and it must be documented in the callback table (§37).
- Lock-order hazard inherited from the framework: never take
  `node->nd_input_queue.q_mtx` (framework-owned; order is node queue → worklist,
  `ng_base.c:2034-2037`).
- Feedback-loop hazard: guard response loops with `NG_HOOK_SET_TO_INBOUND(hook)`
  (`netgraph.h:350`) plus `NG_OUTBOUND_THREAD_REF()` (`netgraph.h:1215`), as
  `ng_ether.c:287-289` does.

---

## 15. Timer / callout model (§49)

Every timer is owned by the node and created in the constructor, destroyed in
`shutdown`.

| Timer | Created | Armed by | Callback | Context | Cancellation |
|---|---|---|---|---|---|
| `t_rx_gap` (RTU t1.5) | `callout_init(c, CALLOUT_TAILACTIVE)` in ctor | `callout_reset_sbt(..., ustosbt(t15), rx_gap_cb, ...)` from `rcvdata` | softclk → `ng_send_fn(node, hook, rx_gap_fn, priv, 0)` | softclk, touches only the callout arg | `callout_drain(c)` in `shutdown` |
| `t_tx_turnaround` (RTU t3.5 before transmitting) | same | `rcvmsg`/`rcvdata` after a good frame | softclk → `ng_send_fn(...)` | softclk | `callout_drain()` |
| `t_transaction` (client response timeout) | same | client send | softclk → `ng_send_fn(...)` | softclk | `callout_drain()` |

Teardown order (§16, §49): `close` (optional) → all hooks destroyed by the
framework → input queue flushed → **our `shutdown`**: drain every callout with
`callout_drain()` (blocks until not running), free backend, free pending
buffers, `NG_NODE_SET_PRIVATE(node, NULL)`, `NG_NODE_UNREF(node)`.
Because every timer callback only enqueues an `ng_send_fn` and every queued fn
holds a node reference (`ng_base.c:3716-3743`), no callback can ever touch
freed state — this is the invariant to state in the source comments.

For node-internal, non-time work use `ng_send_fn()` (writer context) rather than
raw taskq, so it inherits the node gate.

---

## 16. Node lifecycle model

Framework order (`ng_rmnode`, `ng_base.c:714-786`):

1. extra `NG_NODE_REF`; `NGF_INVALID|NGF_CLOSING` set (`:729-737`)
2. `close()` — first and only call (`:740-741`)
3. **every hook destroyed**, each firing our `disconnect` (`:744-745`)
4. input queue flushed; queued `apply` callbacks get `ENOENT` (`:754`)
5. **`shutdown()`** — this *is* the node destructor; return value ignored
   (`:757-758`)
6. unhash + final `NG_NODE_UNREF` → free (`:776-785`)

Our callbacks therefore need: `constructor` (allocate, init callouts, set
private, `NG_NODE_FORCE_WRITER`), `newhook` (accept `"lower"`, else `EINVAL`;
`EISCONN` if taken), `connect` (remember peer, `NG_HOOK_FORCE_QUEUE` if we will
take the tty path, as `ng_tty.c:194-199` does), `rcvdata`, `rcvmsg`,
`disconnect` (clear slot; `ng_rmnode_self()` when last hook and
`NG_NODE_IS_VALID`, as `ng_echo.c:113-116`), `shutdown`.

Do not cache a `node_p` or `hook_p` across any call that can drop a reference
(`ng_base.c:2390-2427`); re-fetch with `NG_HOOK_NODE()`, `NG_HOOK_PEER()`.

---

## 17. Module unload model

- Type registration/unregistration is owned by `NETGRAPH_INIT` +
  `ng_mod_event` (`ng_base.c:3090-3138`). Unload while any node exists returns
  `EBUSY` **by the framework** (`ng_base.c:3113-3115`); `ng_rmtype()` has the
  same rule (`:1309-1312`).
- The module loader's veto chain is `linker_file_unload()`
  (`sys/kern/kern_linker.c:721`): securelevel → `kld_unload_try` handlers
  (`:744`) → `module_quiesce()` on every module, MOD_QUIESCE (`:758`) →
  `module_unload()`, MOD_UNLOAD (`:785`) → `SYSUNINIT`. Any non-zero return
  from either aborts the whole unload.
- Therefore, with `NETGRAPH_INIT` only, `kldunload modbus` is clean by
  construction: it can only succeed once every node has been destroyed, and
  every node's `shutdown` has already run. No timers, threads, or allocations
  can survive, because they all live inside per-node private state.
- If we later add a global (module-scope) resource — a global node registry, a
  `uma_zone`, a sysctl — we must write `ng_mod_event` and veto/clean on
  `MOD_UNLOAD`, following `ng_ether.c:810-859` and `ng_pipe.c:993-1013`.
  v1 should have **no** module-scope mutable state, so `mod_event` stays unset.
- Repeated load/unload cycles are therefore a framework property; our test must
  still exercise them (§44 Milestone 1) once a FreeBSD runtime is available.

---

## 18. Test architecture

### 18.1 Three layers (§58)

| Layer | Needs | Mechanism |
|---|---|---|
| Protocol | nothing | `share/tests/modbus`: the protocol sources are compiled as a plain host program, which is why they may not include kernel only headers |
| Netgraph | netgraph only | ATF + `tests/sys/netgraph` harness (`util.[ch]`: `ng_init/ng_mkpeer/ng_connect/ng_send_data/ng_send_msg/ng_handle_events`), run as root |
| Transport | no hardware | byte-level unit tests of MBAP/RTU/ASCII state machines, including split/coalesce and timeout cases |

The `tests/sys/netgraph` suite is the right home and has direct precedent:
`tests/sys/netgraph/Makefile:1-30` (`LIBADD+= netgraph`, `ATF_TESTS_C` list),
`basic.c` (create `hub`, connect hooks, exchange data), `ksocket.c` (raw
`NgAllocRecvMsg`), and shell tests that explicitly `load_modules netgraph
ng_socket …` (`ng_macfilter_test.sh:48-56`).

### 18.2 Virtual client/server (Milestone 4, §31)

```
ng_modbus:server  "lower" ─┬─ ng_modbus:client "lower"
                          └─ (or a single ng_pipe in between)
```

ATF test: create two nodes, configure role/unit/backend sizes via control
messages, connect `lower`↔`lower`, drive a `read holding registers`
request from the client, assert the response payload on the client side; then
writes; then an out-of-range address and assert `0x83, 0x02`.

### 18.3 Hostile input (§44 Milestone 8)

Fuzz-shaped vectors: truncated ADU, `LEN` = 0/1/65535, `PID != 0`, FC = 0, FC =
0x80+, byte count inconsistent with quantity, quantity 0 and quantity > limit,
address 0xFFFF with non-zero quantity (wrap), 253-byte max frame, RTU frame with
bad CRC, ASCII frame with bad LRC/odd nibble count, 100 interleaved requests,
disconnect during an outstanding transaction, destroy node with a timer armed,
unload module with a node alive (expect `EBUSY`).

### 18.4 What cannot run here

No FreeBSD kernel on this host, so `kldload`/`kldunload`/`ngctl`/kyua execution
is unavailable. Compile-level verification is available and proven (§3.4).
Options, in order of cost:

1. install `lld` on this host (`apt install lld`) → completes the `.ko` link,
   which at least proves KLD linkage/symbols;
2. run a FreeBSD VM/container elsewhere and `kldload` the built module plus the
   kyua tests → required for Milestone 1 acceptance and all §48 graph tests;
3. treat compile + review as the interim gate and defer runtime acceptance.

---

## 19. Proposed source tree (§7/§55, trimmed to the minimum for Milestones 1-4)

```
sys/net/modbus/modbus.h          types, limits, exception enum, byte helpers
sys/net/modbus/modbus_pdu.c/.h   canonical PDU parse/validate/encode + MBAP-free
sys/net/modbus/modbus_fc.c/.h    FC table + handlers (01,02,03,04,05,06,0F,10,16,17)
sys/net/modbus/modbus_mem.c/.h   memory backend (folded into modbus_backend.* if small)
sys/net/modbus/modbus_tcp.c/.h   MBAP framing + stream state machine  (Milestone 5)
sys/net/modbus/modbus_rtu.c/.h   RTU state machine + timing          (Milestone 6)
sys/net/modbus/modbus_ascii.c/.h ASCII framing + LRC                 (Milestone 7)
sys/net/modbus/modbus_crc.c/.h   CRC-16 (LRC folded into modbus_ascii.c if small)
sys/net/modbus/modbus_client.c/.h client transaction + timeout       (Milestone 4+)
sys/net/modbus/modbus_server.c/.h server dispatch incl. broadcast
sys/netgraph/ng_modbus.c/.h      netgraph node, hooks, control messages, stats
sys/modules/modbus/Makefile      builds modbus.ko from the above
```

Deliberately *not* created up front (§7): `modbus_stats.c`, `modbus_ioctl.c`,
`modbus_trace.c`, and the separate `modbus_lrc.c` (LRC is ~10 lines and belongs
next to ASCII framing).

Licensing: every file above gets the SPDX BSD-2-Clause header of §3.5, with the
project's copyright holder, and no provenance claim.

---

## 20. Unresolved questions / blockers

**Q1 (blocker for Milestone 1 acceptance).** No FreeBSD runtime on this host,
so `kldload ./modbus.ko` and `kldunload` cannot be executed. **Decision taken:
compile-verify only for now.** Runtime acceptance (Milestone 1 §44, and all of
§48's graph tests) stays open and must be run on a FreeBSD system before the
work is called done.

**Q2 (blocker for producing a `.ko` here).** No `lld` on this host; GNU
`ld.bfd` cannot emit `elf_x86_64_fbsd`. `clang` compiles cleanly (verified).
Compiling is the current gate; producing a loadable `modbus.ko` here needs
`apt install lld-21` or a FreeBSD environment.

**Q3.** This workspace's tree is pruned: `sys/kern/Makefile`, `sys/kern/kern.mk`,
`sys/kern/makesys.mk`, `share/doc/style/`, `tools/style/`, and
`sys/amd64/include/machine/` are absent. Worked around for module builds by
creating the `machine`/`x86` include symlinks the build itself normally creates
plus empty `opt_*.h`. Note the build also synthesizes
`sys/amd64/include/machine/*` from `sys/amd64/include/*` via those symlinks, so
module builds are unaffected; only `buildkernel` and the style documents are.

**Q4. RESOLVED.** Copyright holder is **Pedro Giffuni**; the header uses
`SPDX-License-Identifier: BSD-2-Clause` followed by `Copyright (c) 2026 Pedro
Giffuni` and **no** "All rights reserved" line.

**Q5. RESOLVED.** One KLD, `modbus.ko`, containing the netgraph node and the
protocol core (§7/§8).

**Q6.** Type name `modbus`, KLD name `modbus` — so `kldload modbus` works and
`mkpeer modbus:` matches. (Netgraph does not require the module name and node
type name to differ; `ng_ether.ko`/`ether` is the in-tree example of a
difference, but §8 explicitly asks for `modbus.ko`.)

**Q7.** Default backend sizes and whether reconfiguration is permitted while
transactions are outstanding (proposed: reject with `EBUSY` while a transaction
is active or hooks are connected to a transport).

**Q8.** Whether `ng_ksocket` (and therefore `ng_modbus` in TCP mode) should
depend on `MODULE_DEPEND(modbus, netgraph, ...)` only, or additionally require
`ng_ksocket` for the TCP transport. Proposed: no hard dependency — TCP mode
detects a missing `ng_ksocket` peer at connect time and reports `ENXIO`.

---

## Appendix A — implementation matrix format (§4/§47)

One row per protocol requirement, to be filled during implementation:

```
requirement            | spec §  | file:function                | test
FC 03 read holding     | App 6.3 | modbus_fc.c:fc_read_holding  | fc_test.c:fc03_ok/fc03_qty0/fc03_illegaddr
FC 03 exception 0x02   | App 7   | modbus_fc.c (exc mapping)    | fc_test.c:fc03_addr_range
CRC vector 02 07       | Ser B   | modbus_crc.c:crc16_init     | crc_test.c:known_answer
t3.5 gap               | Ser 2.5.1.1 | modbus_rtu.c:rtu_gap    | rtu_test.c:gap_1750us
MBAP length field      | TCP 3.1.3 | modbus_tcp.c:mbap_parse     | tcp_test.c:split/coalesce/bad_len
```

## Appendix A2 — Phase 2 status (Milestone 1, compile-verified)

Implemented, uncommitted:

```
sys/netgraph/ng_modbus.h        node type, hook name, ABI/control structures
sys/netgraph/ng_modbus.c        node: ctor, newhook, rcvmsg, rcvdata, disconnect, shutdown
sys/net/modbus/modbus.h         transport neutral limits, exception codes, data model
sys/modules/modbus/Makefile     builds modbus.ko
sys/modules/Makefile            +1 line, "modbus" in SUBDIR
```

Decisions already encoded, so later phases stay consistent:

- Node type name and registered module name are both `modbus`.  Because
  `NETGRAPH_INIT()` would register the module as `ng_modbus` while the KLD is
  `modbus.ko`, the node declares its own `moduledata_t` plus
  `DECLARE_MODULE(modbus, …)` and `MODULE_DEPEND(modbus, netgraph, …)`.  The
  trade-off: `ngctl`'s `mkpeer` autoload looks for `ng_modbus`
  (`ng_socket.c:251-282`), so the module must be kldloaded before its node
  type can be created.
- One hook, `"lower"`; role and transport are node configuration, not hooks.
- `NG_NODE_FORCE_WRITER()` in the constructor: the node carries no mutex at all.
- Control ABI is fixed-width only (`uint8_t`/`uint32_t`/`uint64_t`), versioned by
  `NG_MODBUS_ABI_VERSION` and reported by `getinfo`; `setconfig` validates role,
  transport and unit id before anything reaches the protocol layer.
- Modbus exception codes live in `modbus.h` as `modbus_exc_t`, disjoint from
  errno; nothing in the netgraph layer converts one into the other.
- `getinfo` advertises no framing (`transports = 0`, `max_adu = 0`) until a
  framer lands, so a client cannot configure a framing the node cannot speak.
- `_Static_assert`s tie each control-message structure's size to its parse
  table.  The parse table, not the struct, defines the payload length a
  userland client sends; a missing table entry made `setconfig` fail for every
  `ngctl` invocation until it was caught in review.
- `setconfig` rejects `unit_id > MODBUS_UNIT_MAX` (247); 248..255 are reserved
  by the Serial Line guide and can never be addressed.
- Includes `<sys/kernel.h>`: `MALLOC_DEFINE()` and `DECLARE_MODULE()` expand
  through `SYSINIT()`, which is defined there (`sys/sys/kernel.h:284`). Omitting
  it produces a confusing "type specifier missing" error.

Review performed (four independent passes: security/kernel-safety, netgraph
framework contract, performance, duplication/dead code).  Fixed:

- text/ngctl `setconfig` was unusable (parse table omitted `reserved`, so the
  payload was 3 bytes against a 4-byte size check);
- `unit_id` range check was tautological (`> 255` on a `uint8_t`), accepting
  the spec-reserved 248..255 range its own comment said to reject;
- response allocation failure returned success with no reply, leaving the
  control socket waiting for a message that could never arrive;
- the defensive `rcvdata` path returned without freeing the mbuf it had taken;
- `getinfo` advertised TCP/RTU/ASCII framing that does not exist and an `max_adu`
  of 260 that is smaller than the ASCII frame it claimed to support;
- the `reserved` byte was caller-controlled and echoed back;
- the module registered as `ng_modbus` while the KLD is `modbus.ko`.

One agent claim was disproved during verification and dropped: `NG_NODE_FORCE_WRITER`
does apply to inbound items, because `ng_snd_item()` reads the destination node
(`ng_base.c:2245`, set at `:3623`), not the sender.

Verification performed:

- `bmake` (BSD make, as invoked on a FreeBSD system) module build with the
  FreeBSD flags, `-Werror` among them:
  **0 errors, 0 warnings**, `ng_modbus.o` produced. Link to `modbus.ko` fails
  only because `lld` is unavailable here (baseline `ng_deflate` fails
  identically), so the object build is the current gate per decision Q1/Q2.
- Recursive wiring verified: `bmake -f sys/modules/Makefile MODULES_OVERRIDE=modbus`
  descends into `sys/modules/modbus` and builds the module.
- `perl tools/build/checkstyle9.pl -f <three files>`: **0 errors, 0 warnings**.
- Not yet verified (needs FreeBSD): `kldload`, `kldunload`, `ngctl mkpeer
  modbus:`, hook lifecycle, and repeated load/unload.

---

## Appendix A3 — Phase 2 and 3 status (PDU engine and memory backend)

Implemented, uncommitted:

```
sys/net/modbus/modbus.h            limits, exception codes, roles, modbus_ctx
sys/net/modbus/modbus_pdu.c/.h     PDU parse/validate, bounded builder, exceptions
sys/net/modbus/modbus_fc.c/.h      01 02 03 04 05 06 0F 10 16 17 + dispatch
sys/net/modbus/modbus_backend.c/.h memory backed coils/discrete/registers
sys/net/modbus/modbus.c            modbus_handle(): the transport neutral entry
sys/netgraph/ng_modbus.c           data path now runs the engine
share/tests/modbus/               host protocol tests
```

Protocol surface: PDU parse rejects anything outside 1..253 bytes and any
function code outside 1..127; every handler validates length, quantity and byte
count before touching the backend; backend ranges never wrap, a request that
runs off the map answers exception 02; read only areas answer 02 on write;
`modbus_handle()` returns 0 for a normal response, a positive exception code
with an exception PDU, or a negative errno when nothing may be sent.

Transport status: the node still speaks no real framing. With
`transport=none` or `vnet` one received item is one request PDU and the
response PDU comes back on the same hook, which makes the node usable and
testable over plain netgraph without a serial line or TCP.

Verification performed:

- The host protocol checks pass (`share/tests/modbus`, `make run`): valid
  requests for all ten function codes, the quantity limits from each request
  diagram (2000, 2000, 125, 125, 123, 121), byte count mismatches, truncated
  requests, ranges crossing the end of an area, an out of specification coil
  value, the mask write formula, read only areas, unknown function codes,
  function code 0 and oversized requests.
- The kernel build is still clean under `-Werror`, and `checkstyle9.pl` reports
  0 errors and 0 warnings on every file.
- The host build adds `-Wextra`, which the kernel flag set does not use. It
  found a signed comparison in `modbus_pdu_buf_append()` that could have made
  the bounds check meaningless; fixed.
- The core is compiled outside the kernel as well as inside it: it includes
  its own headers with quotes, uses `memmove` and `memset` rather than
  `bcopy`, keeps its allocations behind `modbus_alloc()` because the kernel
  and the C library take the malloc type and the flags in a different order,
  and picks the two headers that differ between the two worlds
  (`<sys/systm.h>` and the byte order helpers) in `modbus_sys.h`.  No shim
  headers are shipped.

Not verified: no FreeBSD runtime, so the node has still never been loaded, and
the frameless data path has not been exercised through netgraph.

Still owed: Modbus/TCP (Milestone 5), RTU with CRC and t1.5/t3.5 timing
(Milestone 6), ASCII with LRC (Milestone 7), the kyua/ATF netgraph tests, the
`ng_modbus(4)` man page, and the compliance and function-code matrices.

---

## Appendix A4 — Phase 4 to 7 status (framings wired into the node)

New, uncommitted:

```
sys/net/modbus/modbus_crc.c/.h    CRC-16, poly 0xa001, init 0xffff
sys/net/modbus/modbus_rtu.c/.h    RTU state machine and character times
sys/net/modbus/modbus_ascii.c/.h  ASCII framing and LRC
sys/net/modbus/modbus_tcp.c/.h    MBAP stream parser and encoder
share/tests/modbus/framing_test.c framing tests
```

Design: each framing is a pure state machine fed one byte at a time with an
injected timestamp.  None of them allocates, owns a timer, or knows about
mbufs or hooks, which is what makes them testable on a host with no serial
line and no TCP stack, and what keeps the timing policy in the node.

Timing: `modbus_rtu_t15_ns()` and `modbus_rtu_t35_ns()` compute 1.5 and 3.5
character times from the configured baud and bits per character, and use the
fixed 750 us and 1750 us the specification prescribes above 19200 Bd.  The
node drives the parser with a synthetic timestamp for now; the real deadline
will come from a callout armed on the first byte of a frame.

Responses are framed the same way they arrived: MBAP for TCP with the request
transaction identifier, address plus CRC for RTU, colon plus hex plus LRC plus
CR LF for ASCII, bare PDU for the frameless transport.  A Modbus/TCP request
for another unit is ignored, and a broadcast RTU request is executed but not
answered, per section 2.2 of the Serial Line guide.

Verification: 105 checks pass on the host.  The framing tests cover the
appendix B CRC vector, LRC, character times on both sides of 19200 Bd, RTU
frame completion by CRC, a corrupted CRC, a frame abandoned by silence, an
oversized run, ASCII encode and decode, a bad LRC, a non hexadecimal digit, a
second colon, an odd digit count, and Modbus/TCP byte at a time reassembly,
two coalesced requests, a non zero protocol identifier, an oversized length
field and a length field of one.  The kernel build is clean under `-Werror`
and every file is clean under `checkstyle9.pl`.

Three engine bugs were found by these tests and fixed, which is the argument
for having the harness at all:

- the ASCII decoder treated a second colon as a malformed character instead of
  abandoning the frame and starting the next one;
- the MBAP parser computed the frame size as 7 plus the length field, but the
  length field already counts the unit identifier, so every frame was expected
  to be one byte too long;
- the MBAP header was validated one byte late, after the unit identifier had
  already been buffered.

Still owed: client role with one outstanding transaction and a timeout,
kyua/ATF netgraph tests, the `ng_modbus(4)` man page, the compliance and
function-code matrices, and any runtime evidence at all.

---

## Appendix A5 — Phase 4 remainder, documentation and tests

Client role:

```
sys/net/modbus/modbus_client.c/.h   one outstanding transaction, no timer
```

`modbus_client_start()` refuses a second concurrent request, `modbus_client_input()`
matches a response by function code including the exception form and ignores
anything else, and `modbus_client_expired()` gives the transaction up.  The node
exposes it through the `request` and `getresponse` control messages and does not
retry or arm a timer yet.

Documentation:

```
share/man/man4/ng_modbus.4           node, hooks, configuration, function codes,
                                     statistics, examples, caveats
share/man/man4/Makefile              +1 line
share/tests/modbus/COMPLIANCE.md   specification to file:function to test matrix
                                     for every framing rule and function code
```

Netgraph tests:

```
tests/sys/netgraph/ng_modbus.c       lifecycle, text control interface, virtual
                                     client and server, peer removal, client request
tests/sys/netgraph/Makefile          +2 lines
```

These cannot be compiled or run on this host: they need FreeBSD headers, atf and
kyua, and a root context.  They follow the harness in `tests/sys/netgraph/util.h`
and `basic.c`, and each case loads the module with `kldload -n modbus` because the
node cannot be autoloaded by type name.

Verification performed:

- 128 host checks pass, the 23 new ones covering the client transaction state.
- The kernel build is clean under `-Werror`; `checkstyle9.pl` reports 0 errors and
  0 warnings on every new and modified file, including the kyua test.

Timers, added after review:

- `client_timer` expires an outstanding transaction after
  `MODBUS_CLIENT_TIMEOUT_MS` and counts it in `stats.timeouts`;
- `rtu_timer` discards a frame that is still incomplete after 3.5 character
  times of real silence.

Both are `struct callout` owned by the node, created in the constructor and
`callout_drain()`ed in shutdown.  Their handlers run in callout context and
touch nothing but the node pointer: they re-enter through `ng_send_fn()`, which
queues a function item, so the work happens in the writer class with the node
reference held by the item.  The RTU parser is now driven by `getnanouptime()`
rather than a per-item counter, so the inter-character rule sees real gaps.

Two review passes over the engine, the framings and the node found seven defects
that the tests had not reached.  All are fixed, and each fix is either covered
by a new test or is in the node, which only the kyua suite can run:

- the data path fed the framers only the first mbuf, but `ng_ksocket` delivers
  a stream as a chain whose first mbuf is empty, so the documented Modbus/TCP
  topology parsed nothing at all;
- `modbus.ko` leaked the whole register map on every node destruction;
- a control message could dereference a NULL hook and panic when a client
  request was issued with no lower hook attached;
- the ASCII parser would extend an already-verified frame with the characters
  following it;
- a node configured as unit 0 answered broadcast frames, which would collide on
  a shared bus;
- the write-multiple-coils limit was 2000 instead of the 0x07b0 of section
  6.11, accepting requests a conforming slave must reject;
- `getinfo` reported a zero baud rate although the RTU framer was relying on
  the configured one.

Two findings from the same passes were dropped: one claimed `ng_send_fn` takes a
flags argument in this tree (it does not), and one reported the client role as
unwired, which was already true by the time that agent read the file.

Still owed: kyua runs on a real system, the `.ko` link, any interoperability
run against another implementation, and the first `kldload`.

---

## Appendix B — verified environment facts

- `uname`: Linux 6.18 WSL2 (not FreeBSD); `/boot/kernel` absent.
- Tree: FreeBSD 16.0-CURRENT, git branch `ng_modbus`, HEAD `43b0384bc7e`.
- Available tooling: `clang` 21, `bmake`, `perl`, `kyua` (no FreeBSD runtime),
  `curl`, `python3`; **no** `rg`, no `lld`, no `sudo`.
- Validated build command: see §3.4; `ng_echo.c` compiles with **0 errors**
  under FreeBSD's real `-Werror` warning set using the same harness that
  `modbus.ko` will use.
- Style checker runs locally: `perl tools/build/checkstyle9.pl -f <file>`
  (perl present).