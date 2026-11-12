#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Thin out vector files recorded by hostdiff -G / x87trace -G.

usage: sample_vectors.py OUT CAP IN...

Keeps every transcendental case and at most CAP cases per other encoding
(evenly spread over the input), so that a small file still covers every
instruction form that was recorded.
"""
import sys
from collections import defaultdict

TRANS = {0xF0, 0xF1, 0xF2, 0xF3, 0xF9, 0xFB, 0xFE, 0xFF}

def records(path):
    data = open(path, "rb").read()
    pos = 0
    while pos + 12 <= len(data):
        msz = data[pos + 10]
        size = 12 + 2 * (108 + msz)
        yield data[pos:pos + size]
        pos += size

def main():
    out, cap = sys.argv[1], int(sys.argv[2])
    groups = defaultdict(list)
    seen = set()
    for path in sys.argv[3:]:
        for rec in records(path):
            if rec in seen:
                continue
            seen.add(rec)
            groups[(rec[0], rec[1], rec[2])].append(rec)
    kept = 0
    with open(out, "wb") as f:
        for key in sorted(groups):
            recs = groups[key]
            if not (key[0] == 0xD9 and key[1] in TRANS) and len(recs) > cap:
                step = len(recs) / cap
                recs = [recs[int(i * step)] for i in range(cap)]
            for rec in recs:
                f.write(rec)
            kept += len(recs)
    print("%s: %d cases, %d encodings" % (out, kept, len(groups)))

if __name__ == "__main__":
    main()
