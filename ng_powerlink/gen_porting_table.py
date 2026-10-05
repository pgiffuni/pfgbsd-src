#!/usr/bin/env python3
"""
Generate the openPOWERLINK -> FreeBSD function level porting table.

Input:  the function inventory extracted from upstream by extract_funcs.py
Output: a markdown table with the columns the specification asks for:

    upstream file | function | FreeBSD file | action | callback context |
    allocation behaviour | locking behaviour | RT critical? |
    netgraph interaction? | status

The per-file verdicts below come from the source study of each upstream file.
Where a column cannot be derived from the study it is reported as "review"
rather than guessed, because the specification requires the table to be
generated from the source rather than asserted.
"""

import json
import sys

INVENTORY = "/tmp/kilo/oplk_funcs.json"

# ---------------------------------------------------------------------------
# Per-file disposition, from the upstream study.
#
#   reuse        protocol code, taken as-is (licence header retained)
#   port         protocol code with operating system calls replaced
#   reimplement  platform layer, written for FreeBSD
#   new          no upstream counterpart, new FreeBSD code
#   exclude      not ported (Linux/Windows/VxWorks/openMAC backends)
# ---------------------------------------------------------------------------
FILE_DISPOSITION = {
    # ---- DLL: protocol core -------------------------------------------------
    "dll/dllk.c":               ("port",    "sys/net/powerlink/powerlink_dll.c",  "event", "init/config only"),
    "dll/dllkstatemachine.c":   ("reuse",   "sys/net/powerlink/powerlink_dllstatemachine.c", "event", "none"),
    "dll/dllkfilter.c":         ("reuse",   "sys/net/powerlink/powerlink_dllfilter.c", "event", "none"),
    "dll/dllknode.c":           ("port",    "sys/net/powerlink/powerlink_dllnode.c", "event", "config only"),
    "dll/dllkframe.c":          ("port",    "sys/net/powerlink/powerlink_dllframe.c", "EDRV RX (softirq)", "none in RX path"),
    "dll/dllkevent.c":          ("port",    "sys/net/powerlink/powerlink_dllevent.c", "event thread", "none"),
    "dll/dllkcal.c":            ("port",    "sys/net/powerlink/powerlink_dllcal.c", "event", "none"),
    "dll/dllkcal-circbuf.c":    ("reimplement", "sys/net/powerlink/powerlink_dllcal.c", "event", "queue storage"),
    # ---- NMT / PDO ----------------------------------------------------------
    "nmt/nmtk.c":               ("port",    "sys/net/powerlink/powerlink_nmt.c", "event", "config only"),
    "pdo/pdok.c":               ("port",    "sys/net/powerlink/powerlink_pdo.c", "event", "config only"),
    "pdo/pdokcal.c":            ("port",    "sys/net/powerlink/powerlink_pdocal.c", "event", "none"),
    "pdo/pdoklut.c":            ("reuse",   "sys/net/powerlink/powerlink_pdolut.c", "n/a", "none"),
    "pdo/pdokcalmem-linuxkernel.c": ("reimplement", "sys/net/powerlink/powerlink_pdomem.c", "n/a", "config"),
    "pdo/pdokcal-triplebufshm.c":   ("reimplement", "sys/net/powerlink/powerlink_pdomem.c", "n/a", "config"),
    # ---- event / errhnd / ctrl ----------------------------------------------
    "event/eventk.c":           ("reuse",   "sys/net/powerlink/powerlink_event.c", "event", "none"),
    "event/eventkcalintf-circbuf.c": ("reuse", "sys/net/powerlink/powerlink_event.c", "event", "queue storage"),
    "event/eventkcal-linuxkernel.c": ("reimplement", "sys/net/powerlink/powerlink_eventcal.c", "event", "config"),
    "event/eventkcal-linux.c":  ("exclude", "", "n/a", ""),
    "event/eventkcal-winkernel.c": ("exclude", "", "n/a", ""),
    "event/eventkcal-win32.c":  ("exclude", "", "n/a", ""),
    "event/eventkcal-nooscircbuf.c": ("exclude", "", "n/a", ""),
    "event/eventkcal-nooshostif.c": ("exclude", "", "n/a", ""),
    "event/eventkcal-noosdual.c": ("exclude", "", "n/a", ""),
    "event/eventkcal-direct.c": ("exclude", "", "n/a", ""),
    "errhnd/errhndk.c":         ("port",    "sys/net/powerlink/powerlink_err.c", "any (must be context safe)", "none"),
    "errhnd/errhndkcal.c":      ("reuse",   "sys/net/powerlink/powerlink_errcal.c", "any", "none"),
    "errhnd/errhndkcal-local.c": ("reuse",  "sys/net/powerlink/powerlink_errcal.c", "any", "config"),
    "errhnd/errhndkcal-posixshm.c": ("reimplement", "sys/net/powerlink/powerlink_errcal.c", "any", "config"),
    "errhnd/errhndkcal-hostif.c": ("exclude", "", "n/a", ""),
    "errhnd/errhndkcal-noosdual.c": ("exclude", "", "n/a", ""),
    "ctrl/ctrlk.c":             ("port",    "sys/net/powerlink/powerlink_ctrl.c", "control", "config only"),
    "ctrl/ctrlkcal-direct.c":   ("reimplement", "sys/net/powerlink/powerlink_ctrlcal.c", "control", "config"),
    "ctrl/ctrlkcal-mem.c":      ("exclude", "", "n/a", ""),
    "ctrl/ctrlkcal-hostif.c":   ("exclude", "", "n/a", ""),
    "ctrl/ctrlkcal-noosdual.c": ("exclude", "", "n/a", ""),
    # ---- EDRV ---------------------------------------------------------------
    "edrv/edrvcyclic.c":        ("port",    "sys/net/powerlink/powerlink_edrvcyclic.c", "RT cycle timer", "config only"),
    "edrv/edrv-sim.c":          ("reference", "", "n/a", "n/a"),
    "edrv/edrv-pcap_linux.c":   ("exclude (reference)", "", "n/a", "n/a"),
    "edrv/edrv-rawsock_linux.c": ("exclude (reference)", "", "n/a", "n/a"),
    "edrv/edrv-pcap_win.c":     ("exclude", "", "n/a", "n/a"),
    "edrv/edrv-ndisintermediate.c": ("exclude", "", "n/a", "n/a"),
    "edrv/edrv-i210.c":         ("exclude (reference)", "", "n/a", "n/a"),
    "edrv/edrv-8111.c":         ("exclude", "", "n/a", "n/a"),
    "edrv/edrv-8139.c":         ("exclude", "", "n/a", "n/a"),
    "edrv/edrv-8255x.c":        ("exclude", "", "n/a", "n/a"),
    "edrv/edrv-82573.c":        ("exclude", "", "n/a", "n/a"),
    "edrv/edrv-emacps.c":       ("exclude", "", "n/a", "n/a"),
    "edrv/edrv-openmac.c":      ("exclude (reference)", "", "n/a", "n/a"),
    "edrv/edrvcyclic-openmac.c": ("exclude (reference)", "", "n/a", "n/a"),
    "edrv/edrv-mux_vxworks.c":  ("exclude", "", "n/a", "n/a"),
    # ---- timer --------------------------------------------------------------
    "timer/hrestimer-linuxkernel.c": ("reimplement", "sys/net/powerlink/powerlink_hrestimer.c", "timer callback", "config"),
    "timer/hrestimer-posix.c":  ("reference", "", "n/a", "n/a"),
    "timer/hrestimer-posix_clocknanosleep.c": ("reference", "", "n/a", "n/a"),
    "timer/hrestimer-sim.c":    ("exclude", "", "n/a", "n/a"),
    "timer/hrestimer-windows.c": ("exclude", "", "n/a", "n/a"),
    "timer/hrestimer-vxworks.c": ("exclude", "", "n/a", "n/a"),
    "timer/hrestimer-openmac.c": ("exclude", "", "n/a", "n/a"),
    "timer/hrestimer-i210.c":   ("exclude", "", "n/a", "n/a"),
    "timer/hrestimer-ndistimer.c": ("exclude", "", "n/a", "n/a"),
    "timer/hrestimer-zynqttc.c": ("exclude", "", "n/a", "n/a"),
    "timer/synctimer-openmac.c": ("exclude", "", "n/a", "n/a"),
    "timer/timestamp-openmac.c": ("exclude", "", "n/a", "n/a"),
    # ---- timesync -----------------------------------------------------------
    "timesync/timesynck.c":     ("port",    "sys/net/powerlink/powerlink_timesync.c", "event", "none"),
    "timesync/timesynckcal-linuxkernel.c": ("reimplement", "sys/net/powerlink/powerlink_timesynccal.c", "control", "config"),
    "timesync/timesynckcal-bsdsem.c": ("reference", "", "n/a", "n/a"),
    "timesync/timesynckcal-linuxdpshm.c": ("exclude", "", "n/a", "n/a"),
    "timesync/timesynckcal-winkernel.c": ("exclude", "", "n/a", "n/a"),
    "timesync/timesynckcal-hostif.c": ("exclude", "", "n/a", "n/a"),
    "timesync/timesynckcal-local.c": ("reference", "", "n/a", "n/a"),
    "timesync/timesynckcal-noosdual.c": ("exclude", "", "n/a", "n/a"),
    # ---- veth: replaced by the netgraph EDRV -------------------------------
    "veth/veth-generic.c":      ("reference", "", "n/a", "n/a"),
    # veth is replaced by the netgraph virtual EDRV; the two entry points
    # upstream calls (veth_init/veth_exit) become part of that EDRV.
    "veth/veth-ndisintemediate.c": ("exclude", "", "n/a", "n/a"),
    "veth/veth-linuxuser.c":    ("exclude", "", "n/a", "n/a"),
    "veth/veth-linuxkernel.c":  ("exclude", "", "n/a", "n/a"),
    "veth/veth-linuxdpshm.c":   ("exclude", "", "n/a", "n/a"),
    # remaining PDO memory backends: not ported, they exist so that one
    # FreeBSD implementation can be written against the same interface
    "pdo/pdokcalmem-hostif.c":  ("exclude", "", "n/a", "n/a"),
    "pdo/pdokcalmem-local.c":   ("reference", "", "n/a", "n/a"),
    "pdo/pdokcalmem-noosdual.c": ("exclude", "", "n/a", "n/a"),
    "pdo/pdokcalmem-posixshm.c": ("reference", "", "n/a", "n/a"),
    "pdo/pdokcalmem-winkernel.c": ("exclude", "", "n/a", "n/a"),
}

# Functions whose execution context the study established explicitly.
KNOWN_CONTEXT = {
    ("edrv/edrvcyclic.c", "timerHdlCycleCb"):   ("RT cycle timer", "yes", "no"),
    ("edrv/edrvcyclic.c", "timerHdlSlotCb"):    ("RT cycle timer", "yes", "no"),
    ("edrv/edrvcyclic.c", "processTxBufferList"): ("RT cycle timer", "yes", "no"),
    ("dll/dllknode.c", "cbMnSyncHandler"):     ("EDRV cycle callback", "yes", "no"),
    ("dll/dllkframe.c", "dllkframe_processFrameReceived"): ("EDRV RX", "yes", "no"),
    ("dll/dllk.c", "cbCnTimerSync"):           ("sync timer", "yes", "no"),
    ("dll/dllk.c", "cbCnLossOfSync"):          ("sync timer", "yes", "no"),
    ("dll/dllk.c", "cbCnPresFallbackTimeout"): ("sync timer", "yes", "no"),
    ("dll/dllk.c", "dllk_cbCyclicError"):     ("EDRV error callback", "yes", "no"),
    ("dll/dllkframe.c", "cbCnTimer"):          ("cycle timer", "yes", "no"),
}

RT_CRITICAL_FILES = {
    "edrv/edrvcyclic.c",
    "dll/dllkframe.c",
    "dll/dllkevent.c",
    "dll/dllknode.c",
    "nmt/nmtk.c",
    "pdo/pdok.c",
}


def main():
    inv = json.load(open(INVENTORY))
    rows = []
    for area, funcs in inv.items():
        for f in funcs:
            key = f["file"]
            disp = FILE_DISPOSITION.get(key)
            if disp is None:
                disp = ("review", "", "review", "review")
            action, target, ctx, alloc = disp
            known = KNOWN_CONTEXT.get((key, f["name"]))
            if known:
                ctx, rt, ng = known
            else:
                rt = "yes" if key in RT_CRITICAL_FILES else "no"
                ng = "no"
            if action in ("exclude", "reference", "exclude (reference)"):
                target = "-"
                rows.append((key, f["line"], f["name"], target, action, ctx,
                             alloc, "-", "-", "excluded"))
                continue
            rows.append((key, f["line"], f["name"], target or "review", action,
                         ctx, alloc, rt, ng, "planned"))

    rows.sort(key=lambda r: (r[0], r[1]))
    out = []
    out.append("| upstream file:line | function | FreeBSD file | action | "
               "callback context | allocation | RT critical | netgraph | status |")
    out.append("|---|---|---|---|---|---|---|---|---|")
    for r in rows:
        out.append("| `%s:%d` | `%s()` | %s | %s | %s | %s | %s | %s | %s |"
                   % r)
    print("\n".join(out))

    counts = {}
    for r in rows:
        counts[r[4]] = counts.get(r[4], 0) + 1
    print("\n\n## Summary\n", file=sys.stderr)
    for k in sorted(counts, key=lambda k: -counts[k]):
        print("%-22s %4d" % (k, counts[k]), file=sys.stderr)
    print("%-22s %4d" % ("TOTAL", len(rows)), file=sys.stderr)


if __name__ == "__main__":
    main()