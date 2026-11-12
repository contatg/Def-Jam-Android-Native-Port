#!/usr/bin/env python3
"""Clear old run logs out of logs/, keeping the ones the notes point at.

    python scripts/prune-logs.py                 # say what would go (nothing is deleted)
    python scripts/prune-logs.py --delete        # do it
    python scripts/prune-logs.py --days 7 --delete

Every run writes logs/run-<stamp>.log and .log.err, a long one hundreds of
megabytes. They are evidence for PROGRESS.md and the work-log archive, which cite
them by name, so a cited log is kept however old it is. Also kept: anything newer
than --days (default 3), and everything that is not a run log (iterN-*, milestone
copies, regress-*.txt, hang-* dumps, shots/).

A citation is the full name (`run-20261002-110917`) or, in a list that shortens
the later ones (`run-20261002-011708`, `012247`), the six-digit time after a full
name on the same day.
"""
import argparse
import os
import re
import sys
import time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RUN_RE = re.compile(r"^run-(\d{8})-(\d{6})\.log(\.err)?$")


def cited():
    """The run stamps ("YYYYMMDD-HHMMSS") named anywhere in the notes."""
    out = set()
    roots = [os.path.join(REPO, "PROGRESS.md"), os.path.join(REPO, "docs")]
    files = []
    for r in roots:
        if os.path.isfile(r):
            files.append(r)
        else:
            for d, _, names in os.walk(r):
                files += [os.path.join(d, n) for n in names if n.endswith(".md")]
    for f in files:
        with open(f, encoding="utf-8", errors="replace") as fh:
            text = fh.read()
        for m in re.finditer(r"run-(\d{8})-(\d{6})((?:`?,? ?(?:and )?`?\d{6}`?)*)", text):
            day = m.group(1)
            out.add(day + "-" + m.group(2))
            for short in re.findall(r"\d{6}", m.group(3)):
                out.add(day + "-" + short)
    return out


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--days", type=float, default=3, help="keep run logs newer than this (default 3)")
    ap.add_argument("--delete", action="store_true", help="delete; without it this only reports")
    a = ap.parse_args(argv)

    logs = os.path.join(REPO, "logs")
    keep_stamps = cited()
    cutoff = time.time() - a.days * 86400
    go, kept_cited, kept_new, other = [], 0, 0, 0
    for name in sorted(os.listdir(logs)):
        path = os.path.join(logs, name)
        m = RUN_RE.match(name)
        if not m or not os.path.isfile(path):
            other += 1
            continue
        if m.group(1) + "-" + m.group(2) in keep_stamps:
            kept_cited += 1
        elif os.path.getmtime(path) >= cutoff:
            kept_new += 1
        else:
            go.append(path)
    size = sum(os.path.getsize(p) for p in go)
    print("run logs: %d to delete (%.2f GB), %d kept as cited in the notes, %d kept as newer than %g days; "
          "%d other entries untouched" % (len(go), size / 2**30, kept_cited, kept_new, a.days, other))
    if not a.delete:
        print("nothing deleted; pass --delete to do it")
        return 0
    failed = 0
    for p in go:
        try:
            os.remove(p)
        except OSError:
            failed += 1
    print("deleted %d%s" % (len(go) - failed, ", %d could not be removed" % failed if failed else ""))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
