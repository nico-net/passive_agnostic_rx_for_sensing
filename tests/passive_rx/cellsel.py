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
The tag may follow a run-harness timestamp ("123.45 [c1] ...") and colour-sequence leftovers; both are kept in place."""
import re
import sys

CELL_TAG = re.compile(r"^((?:\x1b?\[[0-9;]*m)*(?:\d+(?:\.\d+)? )?)\[c(\d+)\] ")


def split(line):
    """-> (cell or None, line without the tag)."""
    if line.find("[c", 0, 64) < 0:
        return None, line
    m = CELL_TAG.match(line)
    if not m:
        return None, line
    return int(m.group(2)), m.group(1) + line[m.end():]


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
        with open(path, errors="replace") as f:
            return multi_cells(f.readlines())
    except OSError:
        return None
