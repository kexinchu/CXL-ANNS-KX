#!/usr/bin/env python3
"""Pick two disjoint query-id sets from query_10k.fbin."""
import argparse
import json
import random
import struct
import sys


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--queries", required=True)
    ap.add_argument("--n-build", type=int, default=500)
    ap.add_argument("--n-test", type=int, default=500)
    ap.add_argument("--seed", type=int, default=20260904)
    ap.add_argument("--out-build", required=True)
    ap.add_argument("--out-test", required=True)
    ap.add_argument("--out-json", required=True)
    args = ap.parse_args()

    with open(args.queries, "rb") as f:
        nq, dim = struct.unpack("<II", f.read(8))
    ids = list(range(nq))
    rng = random.Random(args.seed)
    rng.shuffle(ids)
    if args.n_build + args.n_test > nq:
        print("need more queries than file", file=sys.stderr)
        return 2
    build = sorted(ids[: args.n_build])
    test = sorted(ids[args.n_build : args.n_build + args.n_test])
    if set(build) & set(test):
        print("FAIL overlap", file=sys.stderr)
        return 2
    for path, arr in ((args.out_build, build), (args.out_test, test)):
        with open(path, "wb") as f:
            f.write(struct.pack(f"<{len(arr)}I", *arr))
    meta = {
        "seed": args.seed,
        "nq_file": nq,
        "dim": dim,
        "n_build": len(build),
        "n_test": len(test),
        "overlap": 0,
        "build_first5": build[:5],
        "test_first5": test[:5],
        "build_path": args.out_build,
        "test_path": args.out_test,
    }
    with open(args.out_json, "w") as f:
        json.dump(meta, f, indent=2)
        f.write("\n")
    print(json.dumps(meta), flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
