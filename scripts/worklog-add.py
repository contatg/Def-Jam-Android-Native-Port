#!/usr/bin/env python3
"""Append an entry to PROGRESS.md section 6 and keep the section short.

    python scripts/worklog-add.py entry.md          # the entry is a file: "- YYYY-MM-DD HH:MM — ..."
    python scripts/worklog-add.py --trim-only       # just move old entries out
    python scripts/worklog-add.py entry.md --keep 12

The work log holds the most recent entries (eight unless --keep says otherwise).
Older ones move, unchanged and oldest first, to the bottom of
docs/worklog/<YYYY-MM>.md, each to the file of its own month. The note at the top
of the section is updated to name the first entry still in PROGRESS.md.

Line endings of every file touched are preserved.
"""
import argparse
import os
import re
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ENTRY_RE = re.compile(r"^- 20\d\d-\d\d-\d\d \d\d:\d\d", re.M)


def read(path):
    with open(path, encoding="utf-8", newline="") as f:
        s = f.read()
    return s.replace("\r\n", "\n"), ("\r\n" if "\r\n" in s else "\n")


def write(path, s, nl):
    with open(path, "w", encoding="utf-8", newline="") as f:
        f.write(s.replace("\n", nl))


def add_and_trim(progress, entry, keep, worklog_dir):
    """Returns (new PROGRESS text, {month: [entries moved]})."""
    if entry:
        j = progress.index("\n## 7.")
        progress = progress[:j].rstrip("\n") + "\n" + entry.strip("\n") + "\n" + progress[j:]
    a = progress.index("## 6. Work log")
    b = progress.index("\n## 7.", a)
    sec = progress[a:b]
    starts = [m.start() for m in ENTRY_RE.finditer(sec)]
    n_move = max(0, len(starts) - keep)
    moved = {}
    if n_move:
        bounds = starts + [len(sec)]
        for k in range(n_move):
            e = sec[bounds[k]:bounds[k + 1]].rstrip("\n")
            moved.setdefault(e[2:9], []).append(e)
        first_kept = sec[starts[n_move]:][2:18]
        sec = sec[:starts[0]] + sec[starts[n_move]:]
        sec = re.sub(r"Everything before [0-9-]+ [0-9:]+ is in `docs/worklog/[^`]*`( \(one file a month\))?",
                     "Everything before " + first_kept + " is in `docs/worklog/` (one file a month)", sec)
        progress = progress[:a] + sec + progress[b:]
    return progress, moved


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("entry", nargs="?", help="a file holding the new entry")
    ap.add_argument("--keep", type=int, default=8, help="entries to leave in PROGRESS.md (default 8)")
    ap.add_argument("--trim-only", action="store_true")
    a = ap.parse_args(argv)
    if not a.entry and not a.trim_only:
        ap.error("give an entry file, or --trim-only")

    entry = ""
    if a.entry:
        entry, _ = read(a.entry)
        if not ENTRY_RE.match(entry.lstrip("\n")):
            sys.exit('the entry must start "- YYYY-MM-DD HH:MM"')

    path = os.path.join(REPO, "PROGRESS.md")
    progress, nl = read(path)
    worklog = os.path.join(REPO, "docs", "worklog")
    progress, moved = add_and_trim(progress, entry, a.keep, worklog)
    if moved:
        os.makedirs(worklog, exist_ok=True)     # the archive is local-only; a fresh clone has no folder
    for month, entries in moved.items():
        w = os.path.join(worklog, month + ".md")
        if os.path.exists(w):
            ws, wnl = read(w)
            ws = ws.rstrip("\n") + "\n\n" + "\n".join(entries) + "\n"
        else:
            wnl = "\n"
            ws = ("# Work log archive, " + month + "\n\nEntries moved out of `PROGRESS.md` §6, oldest first, unchanged.\n\n"
                  + "\n".join(entries) + "\n")
        write(w, ws, wnl)
    write(path, progress, nl)
    print("moved %d, PROGRESS.md is %d lines" % (sum(len(v) for v in moved.values()), progress.count("\n") + 1))
    return 0


if __name__ == "__main__":
    sys.exit(main())
