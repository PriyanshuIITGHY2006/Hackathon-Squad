# MWIS Workbench

A real C++ solver for Maximum Weight Independent Set, plus a local website that
runs it and shows every decision it makes while it makes them.

The engine is not a re-implementation for display purposes: the page spawns the
actual compiled binary, and what you watch is that process's own trace stream.
There is no time limit unless you ask for one.

```
┌──────────┐   graph on stdin    ┌──────────────┐   JSONL trace on stdout   ┌─────────┐
│ browser  │ ──────────────────► │ server.py    │ ────────────────────────► │ browser │
│ controls │                     │ spawns       │        (SSE)              │ panels  │
└──────────┘                     │ mwis_engine  │                           └─────────┘
                                 └──────────────┘
```

## Quick start

```bash
make -C engine            # builds bin/mwis_engine, bin/mwis_exact, bin/mwis_legacy
python3 server/server.py  # http://127.0.0.1:8000  (builds automatically if needed)
```

Open the page, press **Solve**. Nothing else is required — no pip install, no npm,
no CDN, no network access.

## What is in the box

| Path | What it is |
|---|---|
| `engine/mwis_engine.cpp` | the solver: reductions, LP/Nemhauser-Trotter, exact branch & bound, tree DP, ILS, unfolding, self-verification, JSONL tracing |
| `engine/mwis_exact.cpp` | an independent exact search used to certify answers (≤ 64 vertices) |
| `server/server.py` | stdlib-only HTTP server: builds the engine, runs it, streams its trace |
| `web/` | the workbench UI (vanilla JS, no dependencies) |
| `tools/graphs.py` | instance generators shared by the server and the test harness |
| `tests/fuzz.py` | differential correctness harness |
| `solution.cpp` | the original hackathon submission, left untouched |

## The pipeline

Each stage is visible in the **Pipeline** panel as it happens.

1. **Local reductions** — rules R0–R6 applied to a fixpoint over a worklist.
   Each one either decides a vertex or folds it away, and each is applied only
   where it provably preserves an optimal solution.
2. **LP / Nemhauser-Trotter (R7)** — the LP relaxation of MWIS is half-integral.
   Solving it as a min-cut on the bipartite double cover (Dinic max-flow) splits
   the graph into `x*=1` (in), `x*=0` (out) and `x*=½` (undecided). The `x*=½`
   part is the hard kernel.
3. **Component solve** — the kernel is split into connected components and each
   gets the strongest method that fits it:
   - forest → exact O(n) tree DP
   - up to `--exact-threshold` vertices → branch & bound with a greedy
     clique-cover bound, **proven optimal**
   - anything larger → iterated local search (greedy + PROBE + (1,2)- and
     (2,3)-swaps + adaptive perturbation)
4. **Unfold** — fold records are replayed in reverse to rebuild a solution for
   the original graph.
5. **Verify** — every original edge is checked, and the weight is recomputed
   from the original weights and compared against the solver's own running
   total. A mismatch is a non-zero exit code, not a warning.

When every component was solved exactly, the engine reports `optimal: true` and
the page says **proven optimal**. That is a certificate, not a hope.

## Reduction rules

Every rule carries its proof in the source next to the implementation.

| Rule | Condition | Action |
|---|---|---|
| **R0** isolated | `deg(v) = 0` | include `v` |
| **R1** degree-1 | leaf `v` with neighbour `u`; `w(v) ≥ w(u)` | include `v` |
| **R1** N-fold | leaf `v`; `w(v) < w(u)` | fold: `w(u) ← w(u) − w(v)`, offset `+= w(v)` |
| **R2** neighbourhood removal | `w(v) ≥ Σ w(N(v))` | include `v` |
| **R3** simplicial | `N(v)` is a clique and `w(v) ≥ max w(N(v))` | include `v` |
| **R4** domination | `u ∈ N(v)`, `N[u] ⊆ N[v]`, `w(u) ≥ w(v)` | discard `v` |
| **R5** degree-2 V-fold | `a ≁ b`, `max(w(a),w(b)) ≤ w(v) < w(a)+w(b)` | merge `b` into `a`, `w(a) ← w(a)+w(b)−w(v)` |
| **R6** twins | `u ≁ v`, `N(u) = N(v)`, `w(u)+w(v) ≥ Σ w(N(v))` | include both |
| **R7** LP / NT | half-integral LP via max-flow | fix `x*=1` in, `x*=0` out |

Any rule can be switched off in the UI (or with `--disable`) to see what it was
contributing. Switching all of them off still produces a verified answer — it
just makes the kernel the whole graph.

## Correctness

The harness runs three solvers on the same random instances and re-verifies
every claimed solution in Python, so no solver can certify itself:

```bash
python3 tests/fuzz.py --trials 3000                 # engine vs independent exact search
python3 tests/fuzz.py --trials 400 --with-legacy    # also runs solution.cpp
```

Failures are separated into **INVALID** (the set is not independent, or its
weight is misstated — always a bug) and **SUBOPTIMAL** (valid but lighter than
the proven optimum).

### Two unsound rules in `solution.cpp`

Both were found by this harness and are reproducible with `--legacy-rules`.
They do not produce invalid sets — the answers are always independent — but they
discard optimal solutions.

**1. The degree-2 triangle rule includes the wrong vertex.**
`solution.cpp` includes the heaviest of `{v, a, b}`. Only `v` is known to have
degree 2; if the heaviest is a neighbour `a`, that vertex may have edges outside
the triangle that an optimal solution needs:

```
v–a, v–b, a–b   plus   a–x   with w(x) ≫ w(a)
```

Including `a` forfeits `x`. The sound form is the simplicial rule (R3): include
`v` only when `w(v) ≥ max(w(a), w(b))`. Otherwise `v` is dominated by the heavier
neighbour and R4 removes it — same reduction, no lost optimum.

**2. The domination rule has the containment backwards.**
`solution.cpp` removes `v` when `N[v] ⊆ N[u]` and `w(u) ≥ w(v)`. The sound
direction is `N[u] ⊆ N[v]`: the *dominating* vertex must have the *smaller*
closed neighbourhood, because the exchange argument replaces `v` with `u` and
needs every neighbour of `u` to already be excluded. With the reverse
containment, `u` can have extra neighbours that an optimal solution uses:

```
v–u, u–x     w(v) = w(u) = 10, w(x) = 100
N[v] = {v,u} ⊆ N[u] = {u,v,x}, w(u) ≥ w(v)  ⇒  solution.cpp discards v
optimum is {v, x} = 110, but after discarding v the best available is {x} = 100
```

There is also a cosmetic third issue: in the post-LP reduction loop the
`dominated` flag is computed and never read (`solution.cpp:278`, g++ warns about
it), so that loop cannot skip to the next vertex after a dominance removal.

### Measured effect

Over random instances across 13 graph families, re-verified independently:

- the corrected engine: **no invalid solutions and no suboptimal solutions**
- the unsound rule variants: valid but suboptimal on roughly **one instance in
  five**
- the `solution.cpp` binary itself: valid but suboptimal on roughly **one
  instance in ten** (its local search recovers some of what the rules threw away)

Exact numbers are in [`tests/RESULTS.md`](tests/RESULTS.md), together with two
minimal counterexamples small enough to check by hand:

- `tests/counterexamples/triangle-rule-6v.txt` — 6 vertices; optimum 200,
  `solution.cpp` returns 156
- `tests/counterexamples/domination-direction-5v.txt` — 5 vertices; optimum 110,
  `solution.cpp` returns 102

Paste either into the workbench's **Paste / load…** box and press **Compare
solvers** to watch the difference. Fresh counterexamples from a fuzz run land in
`tests/cases/` and replay with
`python3 tests/fuzz.py --case tests/cases/<file>.txt`.

## Engine CLI

```
bin/mwis_engine [options] < instance

  --trace                 stream newline-delimited JSON events on stdout
  --input FILE            read the instance from FILE instead of stdin
  --time-limit SEC        wall-clock budget (default 0 = unlimited)
  --stall N               stop a component after N non-improving ILS iterations
                          (default 2000; 0 = never stop on its own)
  --max-iters N           hard cap on ILS iterations per component
  --seed S                RNG seed (default 12345; runs are deterministic)
  --exact-threshold N     certify components up to N vertices by branch & bound
  --exact-nodes N         branch & bound node budget per component
  --no-reduce             skip kernelization entirely (ablation)
  --legacy-rules          use solution.cpp's unsound R3/R4 variants
  --disable R0,R4,...     switch individual rules off
```

Instance format (the same one `solution.cpp` reads):

```
N M
w_1 w_2 ... w_N
u v          ← M lines, 1-indexed, undirected
```

Without `--trace` the output is the judge format: total weight on the first
line, the sorted vertex list on the second.

**Stopping is safe.** `SIGTERM` and `SIGINT` are handled cooperatively: the
search stops at the next checkpoint and the engine still unfolds, verifies and
prints a complete answer. The web UI's Stop button is exactly this, which is why
an unlimited run is not a run you can lose.

## HTTP API

| Endpoint | Purpose |
|---|---|
| `GET /api/health` | build status, engine path, available generators |
| `POST /api/generate` | `{kind,n,seed,density,wmax}` → graph |
| `POST /api/run` | `{graph\|text, options}` → `{run_id, cmd}` |
| `GET /api/events/<id>` | SSE stream of engine events (`?from=N` to resume) |
| `POST /api/stop/<id>` | SIGTERM the run |
| `POST /api/exact` | proven optimum via the independent solver (≤ 64 vertices) |
| `POST /api/compare` | engine vs. legacy rules vs. `solution.cpp` vs. optimum |
| `POST /api/verify` | re-check a solution against a graph |

Options reaching the engine are whitelisted and range-clamped in
`server/server.py`; nothing from the request is passed through to a shell.

## Notes on scale

The node-link view draws up to 4000 vertices / 40000 edges. Past that the solve
still runs and every panel except the drawing stays live — a hairball of 50000
vertices is not a picture worth rendering.

The workbench binds to `127.0.0.1` by default. It executes a local compiler and
local binaries, so serve it on a wider interface only on a network you trust.
