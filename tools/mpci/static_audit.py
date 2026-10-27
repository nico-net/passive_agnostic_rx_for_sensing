#!/usr/bin/env python3
"""Static audit of mutable C/C++ objects for the multi-PCI single-process receiver (plan task A2).

Finds, with a conservative text scanner (comments, strings, preprocessor lines stripped first):
  file_static  file-scope `static` objects
  global       file-scope non-static definitions (extern declarations are not definitions)
  func_static  function-local `static` objects
  tls          anything declared __thread / _Thread_local / thread_local
and writes/checks a classification table (tools/mpci/mpci_statics.tsv).

Usage (from anywhere):
  static_audit.py --write      create/refresh the table; new rows are UNCLASSIFIED (owner by file rule),
                               class/owner/note of existing rows are preserved, stale rows are dropped
  static_audit.py --check      exit 1 if a scanned object is missing from the table (new object); prints a
                               per-file count summary. Intended for ctest.
  static_audit.py --strict     as --check, additionally exit 1 on any UNCLASSIFIED row (NOT enabled in ctest yet:
                               it is the gate once A3-A8 have classified every PER_CELL candidate)
  static_audit.py --summary    print the per-file and per-class counts only
  --macros                     list file-scope macro invocations the scanner skipped (diagnostics)

What counts as const: `static const int x`, `static const char *const p`, constexpr. NOT const: `static const char *p`
(the pointer is mutable), `static const char *names[]` (array of mutable pointers). A converted object
(NR_CELL_STATIC*/NR_CELL_STATIC_INIT/NR_CELL_STATIC_HEAP declaration) is reported as kind file_static with
`converted` in its type column and keeps its table row, so a conversion never makes an object disappear
from the audit.
Limits: objects created by macros (`DEFINE_X(name)`), C++ templates and classes are not seen; macro bodies are skipped.
"""
import argparse
import bisect
import fnmatch
import os
import re
import subprocess
import sys
from collections import Counter, OrderedDict

ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
TABLE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "mpci_statics.tsv")
CLASSES = ("PER_CELL", "PROCESS", "CONST", "TLS", "UNCLASSIFIED")

# Receive-path file set (plan "File structure and ownership", group C), plus read-only G-owned host files.
GLOBS = [
    "openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_*.c",
    "openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_*.c",
    "openair1/PHY/NR_UE_TRANSPORT/nr_pusch_passive_*.c",
    "openair1/PHY/NR_UE_TRANSPORT/nr_pusch_*.c",       # A6: solver, uci_blind, data_aided (per-cell UL state the first glob missed)
    "openair1/PHY/NR_UE_TRANSPORT/nr_ul_*.c",          # A6: nr_ul_nid_bg (n_ID worker), nr_ul_nid_rule
    "openair1/PHY/NR_TRANSPORT/nr_ulsch_demodulation.c",  # A6: UL branch-selection table
    "openair1/PHY/NR_UE_TRANSPORT/nr_pusch_ul_*.c",
    "openair1/PHY/NR_UE_TRANSPORT/nr_passive_*.c",
    "openair1/PHY/NR_UE_TRANSPORT/nr_csirs_disc*.c",
    "openair1/PHY/NR_UE_TRANSPORT/nr_csirs_blind_rt.c",  # DBG2: CSI-RS search / confirm queue (the first glob missed it)
    "openair1/PHY/NR_UE_TRANSPORT/nr_csirs_disc_confirm_internal.h",  # DBG2: included by nr_csirs_blind_rt.c
    "openair1/PHY/NR_UE_TRANSPORT/nr_csirs_monitor.c",   # DBG2: CSI-RS monitor bank
    "openair1/PHY/NR_UE_TRANSPORT/pss_nr.c",             # DBG2: PSS fold of the initial sync
    "openair1/PHY/NR_UE_TRANSPORT/dci_nr.c",
    "openair1/PHY/NR_UE_TRANSPORT/nr_dl_traffic_probe*.c",
    "openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_solver.c",
    "openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_recovery.c",
    "openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_gpu_*.c",
    "openair1/PHY/NR_UE_TRANSPORT/nr_gpu_*.c",
    "openair1/PHY/NR_UE_TRANSPORT/*.cu",
    "openair1/PHY/NR_UE_TRANSPORT/*.cuh",
    "openair1/PHY/CODING/nrLDPC_cuda/*.cu",
    "openair1/SCHED_NR_UE/*.c",
    "executables/nr-ue.c",
    "executables/nr-ue-ru.c",                          # A8: per-cell RU table, radio FO
    "openair1/PHY/NR_UE_TRANSPORT/nr_early_start.c",   # A8: early-start progress / slot gate (per cell)
    "openair1/PHY/NR_UE_ESTIMATION/nr_adjust_synch_ue.c",  # A8: timing-loop rebase hand-off (per cell)
    "executables/nr-uesoftmodem.c",
    "openair1/PHY/TOOLS/*.c",
    "openair1/PHY/TOOLS/*.cu",
    "common/utils/nr_core_pool.c",
    "common/utils/nr_core_layout.c",
    "radio/COMMON/*.c",
    "radio/COMMON/*.cu",
]
EXCLUDE_DIRS = ("ref/", "tests/", "/tests/", "/test/", "tests/passive_rx/")


def in_scope(rel):
    if rel.startswith(EXCLUDE_DIRS) or "/tests/" in rel or "/test/" in rel:
        return False
    return not re.search(r"(_test|_tests|test_[^/]*)\.(c|cc|cpp|cu)$", rel)


def files(root):
    out = []
    for g in GLOBS:
        gdir, gpat = os.path.split(g)
        d = os.path.join(root, gdir)
        if not os.path.isdir(d):
            continue
        for f in sorted(os.listdir(d)):
            if fnmatch.fnmatch(f, gpat):
                rel = os.path.join(gdir, f)
                if in_scope(rel) and rel not in out:
                    out.append(rel)
    return sorted(out)


# ------------------------------------------------------------------ text cleaning
def clean(src):
    """Blank comments, string/char literal contents and preprocessor lines; line structure is preserved."""
    out = []
    i, n = 0, len(src)
    bol = True
    while i < n:
        c = src[i]
        if bol:
            j = i
            while j < n and src[j] in " \t":
                j += 1
            if j < n and src[j] == "#":
                out.append(" " * (j - i))  # keep the indentation: offsets in the cleaned text must equal offsets in the source
                # preprocessor line with continuations
                while j < n:
                    if src[j] == "/" and j + 1 < n and src[j + 1] == "*":
                        # a block comment inside a preprocessor line may span lines (A4: apostrophes in it must not open a char literal)
                        k = src.find("*/", j + 2)
                        k = n if k < 0 else k + 2
                        out.append("".join("\n" if ch == "\n" else " " for ch in src[j:k]))
                        j = k
                        continue
                    if src[j] == "\n" and not (j > 0 and src[j - 1] == "\\"):
                        break
                    out.append("\n" if src[j] == "\n" else " ")
                    j += 1
                i = j
                continue
        if c == "/" and i + 1 < n and src[i + 1] == "/":
            while i < n and not (src[i] == "\n" and src[i - 1] != "\\"):
                out.append("\n" if src[i] == "\n" else " ")
                i += 1
            continue
        if c == "/" and i + 1 < n and src[i + 1] == "*":
            j = src.find("*/", i + 2)
            j = n if j < 0 else j + 2
            out.append("".join("\n" if ch == "\n" else " " for ch in src[i:j]))
            i = j
            continue
        if c in "\"'":
            q = c
            j = i + 1
            while j < n and src[j] != q:
                if src[j] == "\\":
                    j += 1
                j += 1
            seg = src[i + 1 : j]
            out.append(q + "".join("\n" if ch == "\n" else " " for ch in seg) + q)
            i = j + 1
            bol = False
            continue
        out.append(c)
        bol = c == "\n"
        i += 1
    return "".join(out)


def match_brace(s, i):
    """s[i] == '{': return index of the matching '}' (or len-1 when unbalanced)."""
    depth = 0
    for j in range(i, len(s)):
        if s[j] == "{":
            depth += 1
        elif s[j] == "}":
            depth -= 1
            if depth == 0:
                return j
    return len(s) - 1


def collapse(text):
    """Replace every {...} group of a statement by '{}' (declaration shape only)."""
    out, depth = [], 0
    for ch in text:
        if ch == "{":
            if depth == 0:
                out.append("{}")
            depth += 1
        elif ch == "}":
            depth -= 1
        elif depth == 0:
            out.append(ch)
    return "".join(out)


# ------------------------------------------------------------------ declaration parsing
ATTR_WORDS = ("__attribute__", "_Alignas", "alignas", "__declspec", "__aligned", "__asm__", "asm", "__asm")
TLS_WORDS = ("__thread", "_Thread_local", "thread_local")
DEV_WORDS = ("__device__", "__constant__", "__managed__", "__shared__")
STORAGE = ("static", "extern", "inline", "register", "auto", "typedef", "__inline__", "__inline", "__forceinline__",
           "__host__", "__global__") + TLS_WORDS + DEV_WORDS


def strip_balanced_after(text, word):
    out = []
    i = 0
    pat = re.compile(r"\b" + re.escape(word) + r"\b\s*\(")
    while True:
        m = pat.search(text, i)
        if not m:
            out.append(text[i:])
            break
        out.append(text[i : m.start()])
        j = m.end() - 1
        depth = 0
        while j < len(text):
            if text[j] == "(":
                depth += 1
            elif text[j] == ")":
                depth -= 1
                if depth == 0:
                    break
            j += 1
        i = j + 1
    return "".join(out)


def split_top(text, sep):
    parts, depth, cur = [], 0, []
    for ch in text:
        if ch in "([{":
            depth += 1
        elif ch in ")]}":
            depth -= 1
        if ch == sep and depth == 0:
            parts.append("".join(cur))
            cur = []
        else:
            cur.append(ch)
    parts.append("".join(cur))
    return parts


def first_top(text, ch):
    depth = 0
    for i, c in enumerate(text):
        if c in "([{":
            depth += 1
        elif c in ")]}":
            depth -= 1
        elif c == ch and depth == 0:
            return i
    return -1


FUNCPTR = re.compile(r"\(\s*(\*+)\s*((?:(?:const|volatile)\s+)*)(\w+)\s*(?:\[[^\]]*\]\s*)*\)\s*\(")
CELL_CONV = re.compile(r"^(NR_CELL_STATIC(?:_INIT|_HEAP)?)\s*\(\s*([^,]+?)\s*,\s*(\w+)")


def norm(s):
    return re.sub(r"\s+", " ", s).strip()


def parse_statement(text):
    """Parse one declaration statement (braces collapsed, no trailing ';').
    Returns (list of dict(name,type,const,static,tls,extern,conv,device), macro_name_or_None)."""
    t = norm(text)
    if not t:
        return [], None
    for w in ATTR_WORDS:
        t = strip_balanced_after(t, w)
    t = norm(t)
    # converted objects: NR_CELL_STATIC*(...) usages carry no ';' and may be glued to neighbouring statements
    mc = re.search(r"\bNR_CELL_STATIC(?:_INIT|_HEAP)?\s*\(", t)
    if mc:
        j, depth = mc.end() - 1, 0
        while j < len(t):
            if t[j] == "(":
                depth += 1
            elif t[j] == ")":
                depth -= 1
                if depth == 0:
                    break
            j += 1
        m = CELL_CONV.match(t[mc.start():j + 1])
        res = []
        if m:
            res.append(dict(name=m.group(3), type=norm(m.group(2)) + " [converted]", const=False, static=True,
                            tls=False, extern=False, conv=True, device=False))
        for part in (t[:mc.start()], t[j + 1:]):
            if part.strip():
                res += parse_statement(part)[0]
        return res, None
    if re.match(r"^(typedef|_Static_assert|static_assert|using|template\b.*\busing|friend|public:|private:)\b", t):
        return [], None
    if re.match(r"^(struct|union|enum|class)\s*\w*\s*\{\}$", t):
        return [], None
    words = re.findall(r"[A-Za-z_]\w*", t)
    first = words[0] if words else ""
    # split at a macro invocation that has no ';' directly followed by a storage keyword: `FOO(x) static int a`
    mm = re.search(r"\)\s+(?=(?:static|__thread|_Thread_local|thread_local|extern)\b)", t)
    if mm and first not in STORAGE and first not in ("const", "volatile", "struct", "union", "enum", "unsigned", "signed"):
        t = t[mm.end():]
        words = re.findall(r"[A-Za-z_]\w*", t)
    # leading storage words
    flags = dict(static=False, tls=False, extern=False, device=False)
    toks = t
    while True:
        m = re.match(r"\s*(" + "|".join(STORAGE) + r")\b", toks)
        if not m:
            break
        w = m.group(1)
        if w == "static":
            flags["static"] = True
        elif w in TLS_WORDS:
            flags["tls"] = True
        elif w == "extern":
            flags["extern"] = True
        elif w in DEV_WORDS:
            flags["device"] = True
        elif w == "typedef":
            return [], None
        toks = toks[m.end():]
    # storage words may also appear after the type (`int static x`) or after __attribute__ removal: scan the rest
    for w in TLS_WORDS:
        if re.search(r"\b" + w + r"\b", toks):
            flags["tls"] = True
            toks = re.sub(r"\b" + w + r"\b", " ", toks)
    if re.search(r"\bstatic\b", toks):
        flags["static"] = True
        toks = re.sub(r"\bstatic\b", " ", toks)
    for w in DEV_WORDS:
        if re.search(r"\b" + w + r"\b", toks):
            flags["device"] = True
            toks = re.sub(r"\b" + w + r"\b", " ", toks)
    toks = norm(toks)
    if flags["extern"] and "=" not in toks:
        return [], None
    # declarators
    out = []
    base_const = None
    decls = split_top(toks, ",")
    for idx, d in enumerate(decls):
        eq = first_top(d, "=")
        dd = d[:eq] if eq >= 0 else d
        dd = norm(dd)
        if not dd:
            continue
        # strip array dims for name search
        core = dd
        while True:
            c2 = re.sub(r"\s*\[[^\[\]]*\]\s*$", "", core)
            if c2 == core:
                break
            core = c2
        dims = dd[len(core):].strip()
        mf = FUNCPTR.search(core)
        if mf:
            name = mf.group(3)
            const = bool(re.search(r"\bconst\b", mf.group(2)))
            tp = norm(core[: mf.start()] + " (*" + ")" + core[mf.end() - 1 :]) if idx == 0 else "fnptr"
            if idx == 0:
                base_const = False
            out.append(dict(name=name, type=norm(tp + dims), const=const, **flags, conv=False))
            continue
        if first_top(core, "(") >= 0 or "(" in core:
            # function declaration or macro invocation; invocations have no type before the name
            fm = re.match(r"^(\w+)\s*\(", core)
            if idx == 0 and fm:
                return [], fm.group(1)
            continue
        nm = re.search(r"([A-Za-z_]\w*)\s*$", core)
        if not nm:
            continue
        name = nm.group(1)
        prefix = core[: nm.start()].strip()
        if idx == 0:
            if not prefix:
                return [], name  # bare identifier: macro
            star = prefix.find("*")
            base = prefix if star < 0 else prefix[:star]
            base_const = bool(re.search(r"\b(const|constexpr)\b", base))
            ptrs = "" if star < 0 else prefix[star:]
            typ = norm(base + " " + ptrs.replace(" ", "")) if star >= 0 else norm(base)
        else:
            ptrs = prefix
            typ = None
        if "*" in ptrs:
            const = bool(re.search(r"\bconst\b", ptrs[ptrs.rfind("*") :]))
        else:
            const = bool(base_const)
        if idx != 0:
            t0 = out[0]["type"].split("*")[0].strip() if out else ""
            typ = norm(t0 + " " + ptrs.replace(" ", ""))
        out.append(dict(name=name, type=norm(typ + dims), const=const, **flags, conv=False))
    return out, None


# ------------------------------------------------------------------ file scan
class Obj:
    def __init__(self, file, line, symbol, kind, typ, cls_auto):
        self.file, self.line, self.symbol, self.kind, self.type, self.cls_auto = file, line, symbol, kind, typ, cls_auto

    def key(self):
        return (self.file, self.symbol, self.kind)


def function_name(header):
    h = norm(header)
    for w in ATTR_WORDS:
        h = strip_balanced_after(h, w)
    m = re.search(r"([A-Za-z_]\w*)\s*\([^()]*(?:\([^()]*\)[^()]*)*\)\s*(?:const|noexcept|override|\s)*$", h)
    return m.group(1) if m else "?"


FUNC_TAIL = re.compile(r"\)\s*(?:(?:const|noexcept|override|final|volatile)\s*)*$")


def scan_text(rel, src):
    cs = clean(src)
    n = len(cs)
    starts = [0]
    for m in re.finditer(r"\n", cs):
        starts.append(m.end())
    lineof = lambda pos: bisect.bisect_right(starts, pos)
    objs, macros = [], []

    def emit(parsed, pos, scope):
        for d in parsed:
            if d["tls"]:
                kind = "tls"
            elif d["static"] and scope:
                kind = "func_static"
            elif d["static"]:
                kind = "file_static"
            elif scope:
                continue
            else:
                kind = "global"
            sym = (scope + "::" + d["name"]) if (scope and kind in ("func_static", "tls")) else d["name"]
            auto = "CONST" if d["const"] and not d["tls"] and not d["device"] else ("TLS" if d["tls"] else None)
            typ = d["type"] + (" [device]" if d["device"] else "")
            objs.append(Obj(rel, lineof(pos), sym, kind, typ, auto))

    def body(lo, hi, fname):
        """function-local statics in cs[lo:hi]"""
        i = lo
        stmt_start = True
        paren = 0
        while i < hi:
            c = cs[i]
            if c.isspace():
                i += 1
                continue
            if stmt_start and paren == 0:
                m = re.match(r"(?:case\b[^:;{}]*:|default\s*:|\w+\s*:(?!:))?\s*(static|__thread|_Thread_local|thread_local|NR_CELL_STATIC(?:_INIT|_HEAP)?)\b", cs[i : i + 200])
                if m:
                    st = i + m.start(1) if m.start(1) > 0 else i
                    j, depth, pd = st, 0, 0
                    while j < hi:
                        ch = cs[j]
                        if ch == "(":
                            pd += 1
                        elif ch == ")":
                            pd -= 1
                        elif ch == "{":
                            depth += 1
                        elif ch == "}":
                            depth -= 1
                            if depth < 0:
                                break
                        elif ch == ";" and depth == 0 and pd == 0:
                            break
                        j += 1
                    parsed, _ = parse_statement(collapse(cs[st:j]))
                    emit(parsed, st, fname)
                    i = j + 1
                    stmt_start = True
                    continue
            stmt_start = False
            if c == "(":
                paren += 1
            elif c == ")":
                paren -= 1
            elif c in ";{}" and paren == 0:
                stmt_start = True
            i += 1

    i = 0
    transparent = 0
    buf_start = None
    depth = 0
    while i < n:
        c = cs[i]
        if buf_start is None:
            if c.isspace():
                i += 1
                continue
            if c == "}" and transparent > 0:
                transparent -= 1
                i += 1
                continue
            if c == ";":
                i += 1
                continue
            buf_start = i
            depth = 0
        if c in "([":
            depth += 1
        elif c in ")]":
            depth -= 1
        elif c == ";" and depth <= 0:
            parsed, mac = parse_statement(collapse(cs[buf_start:i]))
            emit(parsed, buf_start, None)
            if mac:
                macros.append((rel, lineof(buf_start), mac))
            buf_start = None
            i += 1
            continue
        elif c == "{" and depth <= 0:
            text = norm(cs[buf_start:i])
            tt = text
            for w in ATTR_WORDS:
                tt = strip_balanced_after(tt, w)
            tt = norm(tt)
            j = match_brace(cs, i)
            if "=" in tt and not FUNC_TAIL.search(tt):
                i = j + 1  # initializer; statement continues until ';'
                continue
            if re.search(r"\bnamespace\b", tt) or re.match(r'^(?:extern\s*""|extern\s*"\s*")\s*$', tt) or tt in ('extern ""', 'extern "C"'):
                transparent += 1
                buf_start = None
                i += 1
                continue
            if FUNC_TAIL.search(tt) and not re.match(r"^(struct|union|enum|class)\b", tt):
                body(i + 1, j, function_name(tt))
                buf_start = None
                i = j + 1
                continue
            i = j + 1  # struct/union/enum body inside a declaration
            continue
        elif c == "}" and depth <= 0 and buf_start is not None:
            # stray close (unbalanced #if blocks): drop the statement
            buf_start = None
            i += 1
            continue
        i += 1
    return objs, macros


def dedupe(objs):
    seen, out = {}, []
    for o in objs:
        k = o.key()
        if k in seen:
            if seen[k].type == o.type:
                continue  # same object in two #if branches
            m = 2
            while (o.file, o.symbol + "#%d" % m, o.kind) in seen:
                m += 1
            o.symbol += "#%d" % m
            k = o.key()
        seen[k] = o
        out.append(o)
    return out


def scan_tree(root):
    objs, macros = [], []
    for rel in files(root):
        with open(os.path.join(root, rel), errors="replace") as f:
            o, m = scan_text(rel, f.read())
        objs.extend(dedupe(o))
        macros.extend(m)
    return objs, macros


# ------------------------------------------------------------------ owners and auto-classification
OWNER_RULES = [
    # (glob on repo-relative path, owner task)
    ("common/utils/nr_core_pool.c", "B1"),
    ("common/utils/nr_core_layout.c", "B1"),
    ("radio/COMMON/*", "G5"),
    ("executables/*", "A8"),
    ("openair1/SCHED_NR_UE/*", "A8"),
    ("*/nr_gpu_pass_*", "G3"),
    ("*/nr_pdsch_gpu_*", "G3"),
    ("*/nr_gpu_budget*", "G4"),
    ("*/nr_gpu_chunk*", "G4"),
    ("*/nr_gpu_mem*", "G1"),
    ("*/nr_gpu_*", "G1"),
    ("*/nr_csirs_disc_gpu*", "G4"),
    ("*/nr_pdcch_nid_gpu*", "G4"),
    ("*/nr_pdcch_gpu_*", "G4"),
    ("openair1/PHY/CODING/*", "G1"),
    ("openair1/PHY/TOOLS/nr_host_membudget*", "B3"),
    ("openair1/PHY/TOOLS/nr_admit*", "B3"),
    ("*/nr_csirs_blind_rt.c", "DBG2"),
    ("*/nr_csirs_disc_confirm_internal.h", "DBG2"),
    ("*/nr_csirs_monitor.c", "DBG2"),
    ("*/pss_nr.c", "DBG2"),
    ("*/nr_csirs_disc_feed.c", "A7"),
    ("*/nr_csirs_disc_td.c", "A7"),
    ("*/nr_csirs_disc*", "A7"),
    ("*/nr_pusch_*", "A6"),
    ("*/nr_ul_*", "A6"),
    ("*/nr_ulsch_demodulation.c", "A6"),
    ("*/nr_pdcch_ul_*", "A6"),
    ("*/nr_pdsch_*", "A5"),
    ("*/nr_pdcch_coreset_map.c", "A3"),
    ("*/nr_pdcch_*", "A4"),
    ("*/nr_dl_traffic_probe*", "A4"),
    ("*/nr_passive_*", "A3"),
    ("*/dci_nr.c", "A3"),
    ("openair1/PHY/TOOLS/*", "A8"),
]


def owner_of(rel):
    for g, o in OWNER_RULES:
        if fnmatch.fnmatch(rel, g):
            return o
    return "A8"


PROCESS_FILES = (
    "common/utils/nr_core_pool.c",
    "common/utils/nr_core_layout.c",
    "openair1/PHY/TOOLS/nr_host_membudget.c",
    "openair1/PHY/TOOLS/nr_gpu_mem.c",
    "openair1/PHY/TOOLS/nr_gpu_platform.c",
    "openair1/PHY/TOOLS/nr_gpu_path.c",
    "openair1/PHY/TOOLS/nr_gpu_module.c",
    "openair1/PHY/TOOLS/nr_gpu_budget.c",
    "openair1/PHY/TOOLS/nr_admit.c",
)
PROCESS_SYM = re.compile(r"(?i)(^|_)(log|logger|dashboard|dash)(_|$)")
PASS_SYM = re.compile(r"(?i)(inst|graph|pool|cache)")


def auto_class(o):
    if o.cls_auto:
        return o.cls_auto, "auto" if o.cls_auto == "CONST" else "existing thread-local"
    if o.file in PROCESS_FILES or o.file.startswith("radio/COMMON/"):
        return "PROCESS", "process-wide by design (core pool/layout, budgets, GPU platform/path registry, radio device)"
    base = os.path.basename(o.file)
    if base.startswith("nr_gpu_pass_") and PASS_SYM.search(o.symbol):
        return "PROCESS", "GPU pass instance pool / graph cache: shared by all cells (G3 adds per-cell quotas)"
    if PROCESS_SYM.search(o.symbol.split("::")[-1]):
        return "PROCESS", "log/dashboard state"
    return "UNCLASSIFIED", ""


# ------------------------------------------------------------------ table
HEADER = ["file", "line", "symbol", "kind", "type", "class", "owner", "note"]


def read_table(path):
    rows = OrderedDict()
    if not os.path.exists(path):
        return rows
    with open(path) as f:
        for ln in f:
            if ln.startswith("#") or not ln.strip() or ln.startswith("file\t"):
                continue
            p = ln.rstrip("\n").split("\t")
            p += [""] * (8 - len(p))
            rows[(p[0], p[2], p[3])] = dict(zip(HEADER, p[:8]))
    return rows


def write_table(path, objs, old):
    out = ["# mpci static audit table (tools/mpci/static_audit.py). Columns: " + " ".join(HEADER),
           "# Regenerate with `static_audit.py --write`: class/owner/note of existing rows are preserved.",
           "\t".join(HEADER)]
    for o in sorted(objs, key=lambda o: (o.file, o.line, o.symbol)):
        r = old.get(o.key())
        cls, note = auto_class(o)
        if r:
            if r["class"] != "UNCLASSIFIED" or cls == "UNCLASSIFIED":
                cls, note = r["class"], r["note"]
            owner = r["owner"] or owner_of(o.file)
        else:
            owner = owner_of(o.file)
        out.append("\t".join([o.file, str(o.line), o.symbol, o.kind, o.type.replace("\t", " "), cls, owner, note]))
    with open(path, "w") as f:
        f.write("\n".join(out) + "\n")


def summary(rows, out=sys.stdout):
    per_file = Counter(r["file"] for r in rows.values())
    per_class = Counter(r["class"] for r in rows.values())
    per_file_cls = {}
    for r in rows.values():
        per_file_cls.setdefault(r["file"], Counter())[r["class"]] += 1
    print("per-file counts (total | " + " ".join(CLASSES) + "):", file=out)
    for f in sorted(per_file):
        c = per_file_cls[f]
        print("  %-72s %5d | %s" % (f, per_file[f], " ".join("%s=%d" % (k[:4], c.get(k, 0)) for k in CLASSES)), file=out)
    print("per-class totals: " + ", ".join("%s=%d" % (k, per_class.get(k, 0)) for k in CLASSES) + ", total=%d" % len(rows), file=out)
    cand = Counter(r["owner"] for r in rows.values() if r["class"] == "UNCLASSIFIED")
    print("UNCLASSIFIED by owner: " + ", ".join("%s=%d" % (k, cand[k]) for k in sorted(cand)), file=out)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--root", default=ROOT)
    ap.add_argument("--table", default=TABLE)
    g = ap.add_mutually_exclusive_group()
    g.add_argument("--write", action="store_true")
    g.add_argument("--check", action="store_true")
    g.add_argument("--strict", action="store_true")
    g.add_argument("--summary", action="store_true")
    ap.add_argument("--macros", action="store_true")
    a = ap.parse_args(argv)
    objs, macros = scan_tree(a.root)
    old = read_table(a.table)
    if a.macros:
        for f, l, m in macros:
            print("%s:%d macro-invocation %s" % (f, l, m))
    if a.write:
        write_table(a.table, objs, old)
        summary(read_table(a.table))
        return 0
    if a.summary:
        summary(old)
        return 0
    found = {o.key() for o in objs}
    new = [o for o in objs if o.key() not in old]
    stale = [k for k in old if k not in found]
    summary(old)
    rc = 0
    for o in new:
        print("NEW (not in table): %s:%d %s [%s] %s" % (o.file, o.line, o.symbol, o.kind, o.type), file=sys.stderr)
        rc = 1
    if stale:
        print("note: %d table rows no longer found in the sources (run --write to drop them)" % len(stale))
    if a.strict:
        unc = [r for r in old.values() if r["class"] == "UNCLASSIFIED"]
        for r in unc[:50]:
            print("UNCLASSIFIED: %s:%s %s (owner %s)" % (r["file"], r["line"], r["symbol"], r["owner"]), file=sys.stderr)
        if unc:
            print("strict: %d UNCLASSIFIED rows" % len(unc), file=sys.stderr)
            rc = 1
    return rc


if __name__ == "__main__":
    sys.exit(main())
