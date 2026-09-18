#!/usr/bin/env python3
"""Differential correctness harness for the MWIS engine.

For each random instance it runs up to three solvers and cross-checks them:

  bin/mwis_engine   the new engine (kernelization + exact/ILS)
  bin/mwis_exact    an independent exact search, used as ground truth
  bin/mwis_legacy   the original solution.cpp, compared when --with-legacy

Every claimed solution is re-verified in Python against the original graph, so a
solver cannot certify itself.  Two classes of failure are reported separately:

  INVALID       the claimed set is not independent, or its weight is misstated.
                This is always a bug.
  SUBOPTIMAL    the set is valid but lighter than the proven optimum.  For the
                engine this should not happen on instances it reports as
                optimal; for a heuristic run it is a quality measurement.

Counterexamples are written to tests/cases/ so they can be replayed.

Usage:
    python3 tests/fuzz.py --trials 500
    python3 tests/fuzz.py --trials 200 --with-legacy
    python3 tests/fuzz.py --case tests/cases/legacy-suboptimal-0001.txt
"""

import argparse
import json
import os
import random
import subprocess
import sys
from concurrent.futures import ProcessPoolExecutor, as_completed

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, ROOT)

from tools import graphs  # noqa: E402

BIN = os.path.join(ROOT, "bin")
ENGINE = os.path.join(BIN, "mwis_engine")
EXACT = os.path.join(BIN, "mwis_exact")
LEGACY = os.path.join(BIN, "mwis_legacy")
CASES = os.path.join(HERE, "cases")

# Instance shapes worth stressing: sparse, dense, structured, and the shapes
# where a specific reduction rule is the only thing that fires.
KINDS = ["random", "tree", "path", "cycle", "star", "complete", "bipartite",
         "cliques", "matching", "grid", "geometric", "scalefree", "hard-dense"]


def run_solver(path, text, args=(), timeout=20.0, term_after=None):
    """Run a solver on stdin text. If term_after is set, SIGTERM it at that
    point and still collect its output (the solvers print best-so-far)."""
    proc = subprocess.Popen([path] + list(args), stdin=subprocess.PIPE,
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    limit = term_after if term_after else timeout
    try:
        out, err = proc.communicate(text, timeout=limit)
    except subprocess.TimeoutExpired:
        proc.terminate()
        try:
            out, err = proc.communicate(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()
            out, err = proc.communicate()
    return proc.returncode, out, err


def parse_plain(out):
    """Parse the two-line solver output into (weight, [vertices])."""
    lines = [ln for ln in out.strip().splitlines() if ln.strip()]
    if not lines:
        return None, []
    try:
        weight = int(lines[0].strip())
    except ValueError:
        return None, []
    verts = []
    if len(lines) > 1:
        verts = [int(x) for x in lines[1].split()]
    return weight, verts


def check_one(args):
    kind, n, seed, density, with_legacy, legacy_budget = args
    g = graphs.generate(kind=kind, n=n, seed=seed, density=density)
    text = graphs.to_engine_text(g)
    result = {"kind": kind, "n": n, "seed": seed, "density": density,
              "m": len(g["edges"]), "issues": []}

    # ground truth
    rc, out, err = run_solver(EXACT, text, timeout=60)
    opt, opt_verts = parse_plain(out)
    if opt is None:
        result["issues"].append(("EXACT_FAILED", err.strip()[:200]))
        return result, text
    ok, w, conf = graphs.verify(g, opt_verts)
    if not ok or w != opt:
        result["issues"].append(("EXACT_INVALID", "conflicts=%d w=%d claimed=%d" % (conf, w, opt)))
        return result, text
    result["optimum"] = opt

    # engine under test
    rc, out, err = run_solver(ENGINE, text, args=("--stall", "400"), timeout=60)
    ew, everts = parse_plain(out)
    if ew is None:
        result["issues"].append(("ENGINE_FAILED", (err.strip() or "no output")[:200]))
        return result, text
    ok, w, conf = graphs.verify(g, everts)
    if not ok:
        result["issues"].append(("ENGINE_INVALID", "conflicts=%d" % conf))
    elif w != ew:
        result["issues"].append(("ENGINE_WEIGHT_MISMATCH", "claimed=%d actual=%d" % (ew, w)))
    elif w < opt:
        result["issues"].append(("ENGINE_SUBOPTIMAL", "got=%d opt=%d gap=%d" % (w, opt, opt - w)))
    result["engine"] = w
    if rc != 0:
        result["issues"].append(("ENGINE_EXIT_%d" % rc, err.strip()[:200]))

    # engine with the original's unsound rule variants, to size up their effect
    rc, out, err = run_solver(ENGINE, text, args=("--legacy-rules", "--stall", "400"), timeout=60)
    lw, lverts = parse_plain(out)
    if lw is not None:
        ok, w, conf = graphs.verify(g, lverts)
        result["legacy_rules"] = w if ok else None
        if ok and w < opt:
            result["issues"].append(("LEGACY_RULES_SUBOPTIMAL", "got=%d opt=%d gap=%d" % (w, opt, opt - w)))

    # the original binary itself
    if with_legacy:
        rc, out, err = run_solver(LEGACY, text, timeout=60, term_after=legacy_budget)
        gw, gverts = parse_plain(out)
        if gw is None:
            result["issues"].append(("SOLUTION_CPP_FAILED", (err.strip() or "no output")[:200]))
        else:
            ok, w, conf = graphs.verify(g, gverts)
            result["solution_cpp"] = w if ok else None
            if not ok:
                result["issues"].append(("SOLUTION_CPP_INVALID", "conflicts=%d" % conf))
            elif w != gw:
                result["issues"].append(("SOLUTION_CPP_WEIGHT_MISMATCH", "claimed=%d actual=%d" % (gw, w)))
            elif w < opt:
                result["issues"].append(("SOLUTION_CPP_SUBOPTIMAL", "got=%d opt=%d gap=%d" % (w, opt, opt - w)))

    return result, text


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--trials", type=int, default=500)
    ap.add_argument("--max-n", type=int, default=26)
    ap.add_argument("--min-n", type=int, default=6)
    ap.add_argument("--seed", type=int, default=0)
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 2)
    ap.add_argument("--with-legacy", action="store_true",
                    help="also run the original solution.cpp binary (slower: it is "
                         "stopped with SIGTERM after --legacy-budget seconds)")
    ap.add_argument("--legacy-budget", type=float, default=1.0)
    ap.add_argument("--case", help="replay a single saved instance file")
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args()

    for path in (ENGINE, EXACT):
        if not os.path.exists(path):
            print("missing %s — run: make -C engine" % path)
            return 2
    if args.with_legacy and not os.path.exists(LEGACY):
        print("missing %s — run: make -C engine" % LEGACY)
        return 2
    os.makedirs(CASES, exist_ok=True)

    if args.case:
        text = open(args.case).read()
        g = graphs.parse_engine_text(text)
        rc, out, _ = run_solver(EXACT, text, timeout=120)
        opt, _ = parse_plain(out)
        rc, out, _ = run_solver(ENGINE, text, timeout=120)
        ew, everts = parse_plain(out)
        ok, w, conf = graphs.verify(g, everts)
        print("n=%d m=%d  optimum=%s  engine=%s  valid=%s conflicts=%d"
              % (g["n"], len(g["edges"]), opt, w, ok, conf))
        if args.with_legacy:
            rc, out, _ = run_solver(LEGACY, text, timeout=120, term_after=args.legacy_budget)
            gw, gverts = parse_plain(out)
            ok2, w2, conf2 = graphs.verify(g, gverts)
            print("solution.cpp = %s  valid=%s conflicts=%d" % (w2, ok2, conf2))
        return 0

    rng = random.Random(args.seed)
    jobs = []
    for _ in range(args.trials):
        kind = rng.choice(KINDS)
        n = rng.randint(args.min_n, args.max_n)
        seed = rng.randint(1, 10 ** 9)
        density = rng.choice([0.08, 0.12, 0.2, 0.3, 0.45])
        jobs.append((kind, n, seed, density, args.with_legacy, args.legacy_budget))

    tally = {}
    saved = 0
    gaps = {"ENGINE_SUBOPTIMAL": [], "SOLUTION_CPP_SUBOPTIMAL": [], "LEGACY_RULES_SUBOPTIMAL": []}
    done = 0

    with ProcessPoolExecutor(max_workers=args.jobs) as pool:
        futs = [pool.submit(check_one, j) for j in jobs]
        for fut in as_completed(futs):
            result, text = fut.result()
            done += 1
            if not args.quiet and done % 50 == 0:
                print("  %d/%d checked" % (done, len(jobs)), flush=True)
            for tag, detail in result["issues"]:
                tally[tag] = tally.get(tag, 0) + 1
                if tag in gaps and "gap=" in detail:
                    gaps[tag].append(int(detail.split("gap=")[1]))
                if saved < 40:
                    name = "%s-%04d" % (tag.lower().replace("_", "-"), saved)
                    with open(os.path.join(CASES, name + ".txt"), "w") as fh:
                        fh.write(text)
                    with open(os.path.join(CASES, name + ".json"), "w") as fh:
                        json.dump({"tag": tag, "detail": detail, **
                                   {k: v for k, v in result.items() if k != "issues"}}, fh, indent=2)
                    saved += 1

    print("\n" + "=" * 68)
    print("trials: %d   (n in [%d,%d], %d shapes)" % (args.trials, args.min_n, args.max_n, len(KINDS)))
    print("=" * 68)
    if not tally:
        print("no discrepancies: every engine solution was independent and optimal")
    else:
        for tag in sorted(tally):
            line = "  %-32s %5d" % (tag, tally[tag])
            if tag in gaps and gaps[tag]:
                g = gaps[tag]
                line += "   gap avg %.1f  max %d" % (sum(g) / len(g), max(g))
            print(line)
    hard = [t for t in tally if "INVALID" in t or "MISMATCH" in t or "FAILED" in t
            or t.startswith("ENGINE_SUBOPTIMAL") or t.startswith("ENGINE_EXIT")]
    if saved:
        print("\n%d counterexample(s) written to tests/cases/" % saved)
    print()
    return 1 if hard else 0


if __name__ == "__main__":
    sys.exit(main())
