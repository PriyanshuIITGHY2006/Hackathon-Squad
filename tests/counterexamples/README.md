# Counterexamples

Minimal instances where `solution.cpp`'s reduction rules discard the optimum.
Both are verified in `tests/RESULTS.md`, and both are small enough to check by
hand.

| file | vertices | optimum | `solution.cpp` | rule at fault |
|---|---|---|---|---|
| `triangle-rule-6v.txt` | 6 | 200 | 156 | degree-2 triangle includes the heaviest of the triangle, even when that vertex has edges outside it |
| `domination-direction-5v.txt` | 5 | 110 | 102 | domination tests `N[v] ⊆ N[u]`; the sound direction is `N[u] ⊆ N[v]` |

```bash
./bin/mwis_exact  --input tests/counterexamples/triangle-rule-6v.txt   # 200
./bin/mwis_engine --input tests/counterexamples/triangle-rule-6v.txt   # 200
./bin/mwis_legacy < tests/counterexamples/triangle-rule-6v.txt         # 156
```
