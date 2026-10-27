#!/usr/bin/env python3
"""Cell selection for log scorers (plan 2026-10-23-mpci-main-plan.md, B.8 / decision F5, task D2).

A receiver that runs more than one cell prefixes every line with "[c<k>] " (LOG layer, common/utils/nr_cprintf.h); a single-cell
receiver prints no prefix. The scorers call prepare(lines, cell, name) right after reading a log:
  - no tag in the log                      -> lines returned UNCHANGED (same list object): output is byte-identical to before.
  - tags of one cell only                  -> tags stripped, all lines kept (== the unprefixed log of that cell).
  - tags of several cells, cell is None    -> SystemExit(2): programmatic callers must name the cell. The command-line scorers have no
                                              option for it: they call multi_cells()/log_cells() and score EVERY cell separately.
  - tags of several cells, cell == K       -> only lines tagged [cK], tag stripped. Untagged lines of a multi-cell log (threads
                                              bound to no cell: GPU callbacks) belong to no cell and are dropped.
  - cell K not present in the log          -> SystemExit(2).
The tag follows ANSI colour sequences ("\x1b[0m[c1] \x1b[32m[PHY] ...") and may follow a run-harness timestamp ("123.45 [c1] ..."): the colour
sequences are skipped first, then the tag is matched. A tagged line is returned without the tag and without colour sequences (a timestamp is kept).
Logs are read through read_lines(): gzip is recognised by its magic bytes, not by the suffix.
cell_info(path) -> None for an unprefixed / one-cell log, else {cells, pci, counts, untagged} for the per-cell result blocks:
  pci      {cell: PCI} from the last "MPCI_CELL id=.. slot=K event=add|update .. pci=N" / "MPCI_CELL_SLOT cell=K .. pci=N" line (receiver-wide, untagged
           lines carrying their own cell index), else the first tagged ISAC_METRICS "pci" of the cell. Empty when the log has no such line.
  counts   {cell: counts(tagged lines of that cell)}; untagged = counts(untagged lines): process-wide lines (radio, GPU callbacks, MPCI_*) belong
           to no cell, are never added to a cell and are reported apart, so nothing is counted twice.
Every scorer prints, for EVERY multi-cell log, the header "== cell k (PCI n)" ("== cell k (PCI ?)" when the log does not name the cell),
the counts line, the UNTAGGED line and the JSON keys pci (null when unknown) / counts / untagged. N=1 (untagged / one-cell) output is unchanged."""
import gzip
import re
import sys

ANSI = re.compile(r"\x1b\[[0-9;]*m")
CELL_TAG = re.compile(r"^(?:\x1b?\[[0-9;]*m)*((?:\d+(?:\.\d+)? )?)(?:\x1b?\[[0-9;]*m)*\[c(\d+)\] ")
SLOT_PCI = re.compile(r"MPCI_CELL_SLOT cell=(\d+) .*?\bpci=(\d+)")
CELL_PCI = re.compile(r"MPCI_CELL id=\S+ slot=(\d+) event=(?:add|update) .*?\bpci=(\d+)")
METRICS_PCI = re.compile(r'ISAC_METRICS \{.*?"pci":(\d+)')
EVM = re.compile(r"SENSING: EVMGRANT rnti=0x([0-9a-fA-F]+) .*?\bcrc=(\d)")
COUNT_KEYS = ("crnti_grants", "crnti_crc_ok", "len_locks", "len_locks_dl", "len_locks_ul", "uldci", "si_grants", "syncs", "rxdiscont")


def read_lines(path, text=True):
    """Lines of a plain or gzip file (magic bytes). text: universal-newline str lines (as open(path, errors="replace")), else decoded bytes lines."""
    with open(path, "rb") as f:
        gz = f.read(2) == b"\x1f\x8b"
    if text:
        with (gzip.open(path, "rt", errors="replace") if gz else open(path, errors="replace")) as f:
            return f.readlines()
    with (gzip.open(path, "rb") if gz else open(path, "rb")) as f:
        return [raw.decode("utf-8", "replace") for raw in f]


def split(line):
    """-> (cell or None, line without the tag)."""
    if line.find("[c", 0, 64) < 0:
        return None, line
    m = CELL_TAG.match(line)
    if not m:
        return None, line
    return int(m.group(2)), m.group(1) + ANSI.sub("", line[m.end():])


def cells_in(lines):
    return sorted({k for k, _ in (split(l) for l in lines) if k is not None})


def prepare(lines, cell, name="log"):
    present = cells_in(lines)
    if not present:
        if cell not in (None, 0):
            sys.stderr.write("%s: no cell prefix in this log (single cell, index 0); cell %s does not exist\n" % (name, cell))
            raise SystemExit(2)
        return lines
    if len(present) > 1 and cell is None:
        sys.stderr.write("%s: the log contains several cells (%s); scores of different cells are never mixed: name one cell (score each separately)\n"
                         % (name, ", ".join("c%d" % c for c in present)))
        raise SystemExit(2)
    if cell is not None and cell not in present:
        sys.stderr.write("%s: cell %d is not in this log (cells present: %s)\n" % (name, cell, ", ".join("c%d" % c for c in present)))
        raise SystemExit(2)
    out = []
    for l in lines:
        k, rest = split(l)
        if k is None:
            if len(present) == 1:
                out.append(l)
        elif cell is None or k == cell:
            out.append(rest)
    return out


def multi_cells(lines):
    """Cell indices of a log that carries the tags of MORE than one cell, else None (unprefixed or one-cell logs: score as before)."""
    present = cells_in(lines)
    return present if len(present) > 1 else None


def log_cells(path):
    """multi_cells() of a file; None when the file is missing or unreadable."""
    try:
        return multi_cells(read_lines(path))
    except OSError:
        return None


def counts(lines):
    """Per-cell facts of the lines of one cell: C-RNTI EVMGRANTs (rnti != 0xffff) and those with crc=1, SI grants (rnti == 0xffff),
    "length locked" lines (len_locks = DL "DCI 1_1 length locked" + UL "UL automatic DCI length locked:"), ULDCI, "Got synch", RXDISCONT events.
    The OTA excerpts keep every length-locked / Got synch / RXDISCONT line, so those counts equal the full log; EVMGRANT, CRC ok and ULDCI
    counts are excerpt-only (first 60000 lines + summaries)."""
    c = dict.fromkeys(COUNT_KEYS, 0)
    for l in lines:
        if "\x1b" in l:
            l = ANSI.sub("", l)
        if "EVMGRANT rnti=" in l:
            m = EVM.search(l)
            if m and m.group(1).lower() == "ffff":
                c["si_grants"] += 1
            elif m:
                c["crnti_grants"] += 1
                c["crnti_crc_ok"] += m.group(2) == "1"
        elif "length locked" in l:
            if "DCI 1_1 length locked" in l:
                c["len_locks_dl"] += 1
            elif "UL automatic DCI length locked" in l:
                c["len_locks_ul"] += 1
            c["len_locks"] += 1
        elif "SENSING: ULDCI rnti=" in l:
            c["uldci"] += 1
        elif "Got synch" in l:
            c["syncs"] += 1
        elif "SENSING: RXDISCONT abs_slot=" in l:
            c["rxdiscont"] += 1
    return c


def cell_info(path):
    """See the module docstring. None when the file is unreadable or has the tags of at most one cell."""
    try:
        lines = read_lines(path)
    except OSError:
        return None
    cells = multi_cells(lines)
    if cells is None:
        return None
    pci, first, per, rest = {}, {}, {k: [] for k in cells}, []
    for l in lines:
        k, body = split(l)
        if k is None:
            rest.append(body)
            if "MPCI_CELL" in body:
                body = ANSI.sub("", body)
                m = SLOT_PCI.search(body) or CELL_PCI.search(body)
                if m:
                    pci[int(m.group(1))] = int(m.group(2))
        else:
            per[k].append(body)
            if k not in first and "ISAC_METRICS" in body:
                m = METRICS_PCI.search(body)
                if m:
                    first[k] = int(m.group(1))
    pci = {k: pci.get(k, first.get(k)) for k in cells if k in pci or k in first}
    return dict(cells=cells, pci=pci, counts={k: counts(per[k]) for k in cells}, untagged=counts(rest))


def cell_header(k, pci=None):
    return "== cell %d (PCI %s)" % (k, "?" if pci is None else pci)


def counts_line(c):
    return " ".join("%s=%d" % (k, c[k]) for k in COUNT_KEYS)
