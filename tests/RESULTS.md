# Correctness results

Produced by `tests/fuzz.py`. Every claimed solution is re-verified in Python
against the original graph, so no solver certifies itself. Ground truth comes
from `bin/mwis_exact`, an independent exact search that shares no code path with
the engine's branch & bound.

## Headline

| Run | Trials | Engine invalid | Engine suboptimal | `solution.cpp` suboptimal |
|---|---|---|---|---|
| `--trials 3000 --max-n 26` | 3000 | **0** | **0** | not run |
| `--trials 400 --max-n 24 --with-legacy` | 400 | **0** | **0** | **43** (avg gap 66.5, max 202) |
| `--trials 160 --max-n 22 --with-legacy` | 160 | **0** | **0** | **15** (avg gap 39.7, max 98) |

The engine's unsound-rule mode (`--legacy-rules`, which reproduces
`solution.cpp`'s R3/R4) was suboptimal on 607/3000 and 83/400 — roughly one
instance in five. The `solution.cpp` binary itself trips less often because its
ILS recovers part of what the rules discarded, but it still loses optimal
solutions on about one instance in ten.

No solver in any run produced a set that was not independent. These are lost
optima, not invalid answers.

Reproduce:

```bash
make -C engine
python3 tests/fuzz.py --trials 3000                  # engine vs exact
python3 tests/fuzz.py --trials 400 --with-legacy     # also runs solution.cpp
```

## Minimal counterexamples

Both are in `tests/counterexamples/` and are small enough to check by hand.

### 1. Triangle rule includes the wrong vertex — `triangle-rule-6v.txt`

```
6 9
73 48 79 94 2 62
1 4   1 5   1 6   2 4   2 5   3 4   3 6   4 5   5 6
```

| solver | weight | set |
|---|---|---|
| exact optimum | **200** | {1, 2, 3} |
| `bin/mwis_engine` | **200** | {1, 2, 3} |
| `solution.cpp` | 156 | {4, 6} |

`{1,2,3}` is independent: none of 1–2, 1–3, 2–3 is an edge. Its weight is
73+48+79 = 200. `solution.cpp` returns 156, a 22 % loss on six vertices.

Why: vertex 2 has degree 2 with neighbours 4 and 5, and 4–5 is an edge, so the
triangle rule fires. `solution.cpp` includes the heaviest of {2,4,5}, which is
vertex 4 (weight 94). But vertex 4 also has edges to 1 and 3 — the rest of the
optimal set — so fixing it in forfeits 73+48+79 to gain 94.

The sound rule (R3, simplicial) fires only when the degree-2 vertex is itself at
least as heavy as its neighbours: `w(2)=48 < max(94,2)`, so it does not fire, and
vertex 2 correctly survives to the kernel.

### 2. Domination has the containment backwards — `domination-direction-5v.txt`

```
5 6
10 10 1 1 100
1 2   1 3   1 4   2 3   2 4   2 5
```

| solver | weight | set |
|---|---|---|
| exact optimum | **110** | {1, 5} |
| `bin/mwis_engine` | **110** | {1, 5} |
| `solution.cpp` | 102 | {3, 4, 5} |
| `bin/mwis_engine --legacy-rules` | 102 | {3, 4, 5} |

`solution.cpp` discards vertex 1 because `N[1] = {1,2,3,4} ⊆ N[2] = {1,2,3,4,5}`
and `w(2) ≥ w(1)`. That is the wrong containment direction. The exchange argument
replaces 1 with 2, which requires every neighbour of 2 to be excluded already —
but 2 has the extra neighbour 5 (weight 100), which the optimum uses. The sound
condition is `N[u] ⊆ N[v]`: the dominating vertex must have the *smaller* closed
neighbourhood.

That `--legacy-rules` reproduces `solution.cpp`'s answer exactly (102, same set)
confirms the reproduction is faithful and the rule is the cause.

## Replaying a case

```bash
python3 tests/fuzz.py --case tests/counterexamples/triangle-rule-6v.txt --with-legacy
```

Or paste either instance into the workbench's **Paste / load…** box and press
**Compare solvers**.
