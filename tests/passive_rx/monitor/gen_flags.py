#!/usr/bin/env python3
"""Build isac_flags.json: every ISAC_* environment flag the receiver reads, with a one-line description.

Description sources, best first: the PROJECT_MEMORY.md flag tables (`| ISAC_X | what | default |`), a code comment
that names the flag (same file), the comment next to the getenv() call, the first PROJECT_MEMORY.md sentence that
names it. Flags with none of these keep desc null and show as "unknown" on the Settings page.

  python3 gen_flags.py [REPO_ROOT] > isac_flags.json      (read-only on the repo)
"""
import json
import os
import re
import sys

GETENV = re.compile(r'"(ISAC_[A-Z0-9_]+)"')   # exact string literals: getenv("X") and helpers such as env_int("X", ...)
EXTS = (".c", ".h", ".cpp", ".cu", ".cuh", ".cc")
DIRS = ("openair1", "openair2", "executables", "radio", "common")


def clean(s):
    s = re.sub(r"\s+", " ", s.replace("`", "")).strip(" */|-")
    return (s[:177] + "...") if len(s) > 180 else s


def wordy(c):
    return len(re.findall(r"[A-Za-z]{3,}", c)) >= 4


def comment_text(line):
    s = line.strip()
    for tok in ("//", "/*", "*"):
        if s.startswith(tok):
            return s[len(tok):]
    i = s.find("//")
    if i >= 0:
        return s[i + 2:]
    i = s.find("/*")
    return s[i + 2:] if i >= 0 else None


def scan(root):
    flags, files = {}, {}
    for d in DIRS:
        for dp, _, fns in os.walk(os.path.join(root, d)):
            for fn in fns:
                if fn.endswith(EXTS):
                    p = os.path.join(dp, fn)
                    try:
                        lines = open(p, errors="replace").read().split("\n")
                    except OSError:
                        continue
                    for i, line in enumerate(lines):
                        for name in GETENV.findall(line):
                            flags.setdefault(name, {"desc": None, "src": "%s:%d" % (os.path.relpath(p, root), i + 1), "how": None})
                            files.setdefault(name, []).append((lines, i))
    for name, sites in files.items():
        e = flags[name]
        for lines, i in sites:                              # a comment that defines the flag ("X=N ...", "X: ...")
            for j, l in enumerate(lines):
                c = comment_text(l) if name in l else None
                if c and re.search(re.escape(name) + r"\s*(=|:|\()", c) and wordy(c) and "getenv" not in c:
                    nxt = comment_text(lines[j + 1]) if j + 1 < len(lines) else None
                    e["desc"], e["how"] = clean(c + (" " + nxt if nxt and len(c) < 120 else "")), "code comment"
                    break
            if e["desc"]:
                break
        if not e["desc"]:
            for lines, i in sites:                          # comment on or just above the getenv line
                for j in (i, i - 1, i - 2):
                    c = comment_text(lines[j]) if j >= 0 else None
                    if c and wordy(c) and "getenv" not in c:
                        e["desc"], e["how"] = clean(c), "code comment"
                        break
                if e["desc"]:
                    break
    return flags


def memory(root, flags):
    try:
        text = open(os.path.join(root, "PROJECT_MEMORY.md"), errors="replace").read()
    except OSError:
        return
    for m in re.finditer(r"^\| `(ISAC_[A-Z0-9_]+)(?:=[^`]*)?` \| ([^|]+)\|", text, re.M):
        if m.group(1) in flags:
            flags[m.group(1)].update(desc=clean(m.group(2)), how="PROJECT_MEMORY flag table")
    for name, e in flags.items():
        if e["desc"]:
            continue
        i = text.find(name)
        if i >= 0:   # the sentence around the first mention
            a = max(text.rfind(". ", 0, i), text.rfind("\n", 0, i)) + 1
            b = min(x for x in (text.find(". ", i), text.find("\n", i), len(text)) if x >= 0)
            e["desc"], e["how"] = clean(text[a:b]), "PROJECT_MEMORY mention"


if __name__ == "__main__":
    root = sys.argv[1] if len(sys.argv) > 1 else os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))
    f = scan(root)
    memory(root, f)
    json.dump(dict(sorted(f.items())), sys.stdout, indent=0)
    sys.stdout.write("\n")
