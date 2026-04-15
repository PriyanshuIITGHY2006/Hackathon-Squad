# RESEARCH: Maximum Weight Independent Set (MWIS)
## Hackathon Squad Problem — Complete Technical Reference

---

## 1. Problem Formulation

Given an undirected graph G = (V, E) with node weights w(v) > 0:

- Find S ⊆ V such that **no two nodes in S share an edge** (independent set)
- **Maximize** Σ w(v) for v ∈ S

This is **NP-Hard** in general. For N=200,000 with a 5-minute budget, we use heuristics.

---

## 2. Why It's Hard

| Problem | Complexity |
|---|---|
| Verify a solution | P (check all edges) |
| Find maximum size IS | NP-Hard |
| Find maximum weight IS | NP-Hard |
| Approximate within constant factor | No PTAS for general graphs (unless P=NP) |

Brute force = 2^200,000 subsets. More digits than atoms in the universe.

---

## 3. Algorithm Landscape

### 3.1 Kernelization / Graph Reduction (Most Powerful)

Shrink the graph using provably-correct rules before any search. Each rule preserves the optimal solution.

#### Tier 1 — O(n), Apply Always First

| Rule | Condition | Action |
|---|---|---|
| **Degree-0** | deg(v) = 0 | Include v, remove it |
| **Degree-1** | deg(v) = 1, neighbor u | Include heavier of {v,u}, remove both |
| **Degree-2 Fold** | deg(v) = 2, neighbors a,b, no edge (a,b) | If w(v) ≥ w(a)+w(b): include v, remove {v,a,b}. Else: fold into supernode |
| **Triangle** | v in triangle with a,b | If w(v) ≥ w(a)+w(b): include v, remove all three |

#### Tier 2 — O(n²), Apply After Tier 1

| Rule | Condition | Action |
|---|---|---|
| **Dominance** | N[v] ⊆ N[u] and w(u) ≥ w(v) | Remove v (u dominates v) |
| **LP Reduction (Nemhauser-Trotter 1975)** | Solve LP relaxation | x*=1 → always include; x*=0 → always exclude |
| **Crown** | Hall-violating set via matching | Exclude crown vertices, include head |
| **Clique Neighborhood** | N(v) forms a clique | Compare w(v) vs sum of clique weights |

#### Tier 3 — Expensive but Powerful

| Rule | What |
|---|---|
| **Struction (Gellner et al. 2021)** | Temporarily increases graph size to expose hidden reductions. Up to 100× speedup on some instances. |
| **LP Half-Integrality** | Full LP solve: x ∈ {0, 1/2, 1}. x=1/2 subgraph is the hard kernel. |

**Key insight:** On real-world sparse graphs, kernelization alone reduces the problem by **90–99%**. The remaining kernel is tiny enough for exact solving or fast heuristics.

**Papers:**
- Hespe et al. (2019) — "Scalable Kernelization for Maximum Independent Sets" — ACM JEA
- Lamm et al. (2019) — "Exactly Solving MWIS on Large Real-World Graphs" — ALENEX 2019
- Gellner et al. (2021) — "Boosting Data Reduction Using Increasing Transformations" — arXiv:2008.05180
- arXiv:2412.09303 — Comprehensive survey of all reduction rules (Dec 2024)

---

### 3.2 Greedy Construction Algorithms

Fast O(m log n) baseline. Not optimal but strong starting point.

#### GWMIN (Best Greedy for MWIS)
```
Sort vertices by w(v) / (deg(v) + 1) descending
For each vertex v in order:
    If v is still in graph (not removed):
        Add v to solution
        Remove v and all neighbors from graph
```

**Approximation guarantee:** Achieves weight ≥ Σ w(v)/(d(v)+1), which is a 1/(Δ+1)-approximation.

#### GWMAX Variant
```
Weight function: w(v) / Σ_{u ∈ N+(v)} w(u)
```
Better on instances where neighbor weights matter more than degree.

#### GWMIN2 Variant
```
Weight function: w(v)² / Σ_{u ∈ N+(v)} w(u)
```
Balances own weight vs neighbor weight quadratically.

**Paper:** Sakai et al. (2001) — "A note on greedy algorithms for the maximum weighted independent set problem" — Discrete Applied Mathematics

---

### 3.3 Local Search — Core Engine

#### The (j,k)-Swap Move
Remove j vertices from current solution, add k non-conflicting vertices.
- (1,2)-swap: remove 1, add 2 → net gain
- (2,3)-swap: remove 2, add 3
- (0,1)-swap: add a free vertex (when solution is not maximal)

#### ARW Local Search (Andrade, Resende, Werneck 2012) — Landmark Algorithm

Finding (1,2)-swaps in **O(m) amortized** using:

```cpp
// For each v in solution:
//   tight[v] = non-solution neighbors of v where conflict_count = 1
//              (they can enter solution the moment v leaves)

conflict_count[u] = |{v ∈ S : (u,v) ∈ E}|   // for u ∉ S
tight[v] = {u ∉ S : conflict_count[u] = 1 and (u,v) ∈ E}  // for v ∈ S

// Valid (1,2)-swap: v ∈ S, u1,u2 ∈ tight[v], (u1,u2) ∉ E
// Condition: w(u1) + w(u2) > w(v)
```

Update cost per swap: O(deg(v)) — incremental maintenance.

**Full ARW Loop:**
1. Make S maximal (add all free vertices via (0,1)-swaps)
2. Find and apply all valid (1,2)-swaps  
3. Find and apply all valid (2,3)-swaps ← O(m·Δ) to find
4. If any swap found → go to step 1
5. Terminate at (2,3)-local optimum

**Paper:** Andrade, Resende, Werneck (2012) — "Fast Local Search for the Maximum Independent Set Problem" — J. of Heuristics

---

### 3.4 Iterated Local Search (ILS)

Best practical approach for the 5-minute budget:

```
S = greedy_solution(G)
S = local_search(S)
best = S
perturbation_rate = 0.10

loop until time_limit:
    k = max(3, |S| * perturbation_rate)
    S' = remove_k_random_vertices(S, k)
    S' = greedy_extend(S')     // make maximal again
    S' = local_search(S')
    
    if weight(S') >= weight(S):
        S = S'
        no_improve = 0
    else:
        no_improve++
        if no_improve > 50:
            perturbation_rate = min(0.30, perturbation_rate * 1.2)
            no_improve = 0
    
    if weight(S) > weight(best):
        best = S
        perturbation_rate = 0.10   // reset
```

**Key:** Adaptive perturbation prevents getting stuck in the same local basin.

---

### 3.5 Simulated Annealing

Accept worse solutions with probability exp(Δw / T):

```
T_init: set so initial acceptance ≈ 20-50%
cooling: 0.9995–0.9999 per iteration
Perturbation: (1,2)-swap for small moves; random removal of k vertices for large
```

Good complement to ILS — run SA in parallel on a separate solution.

---

### 3.6 Tabu Search (STABULUS)

Mark recently removed vertices as tabu:
- Tabu tenure: `10 + random(0, |tight| / 4)` iterations
- Aspiration criterion: override tabu if move gives global best

Prevents cycling while still exploring.

---

### 3.7 CHILS — Current State of the Art (Langedal, SEA 2025)

Concurrent Hybrid Iterated Local Search:

```
Maintain P=16 solutions in parallel (OpenMP threads)
Every 10 seconds:
    "Consensus" phase: vertices ALL solutions agree on → fix them
    Re-kernelize the disagreement subgraph
    Each thread continues ILS on the reduced subgraph
```

**Outperforms all prior algorithms** on standard benchmarks.
**GitHub:** https://github.com/KennethLangedal/CHILS

---

### 3.8 Memetic Algorithm — MMWIS (Großmann et al. GECCO 2023)

Combines genetic algorithm with kernelization in the inner loop:

```
Population of solutions
→ Crossover (graph-partitioning based recombination)
→ Local search
→ Re-kernelize the residual subgraph  ← key innovation
→ Extend solution on reduced kernel
→ Replace worst in population
```

Best results on **205/207 benchmark instances** vs all competitors.

---

### 3.9 Special Graph Cases (Polynomial Time)

| Graph Type | Algorithm | Complexity |
|---|---|---|
| Tree / Forest | DP: dp_in[v], dp_out[v] | O(n) |
| Bipartite | König's theorem → min-cut | O(n√n + m) |
| Interval graph | Sort by right endpoint + DP | O(n log n) |
| Chordal graph | Perfect elimination ordering + DP | O(n + m) |
| Path | DP | O(n) |
| Cycle | DP (two cases) | O(n) |

**Always check for special structure before heuristics.**

---

## 4. Data Structures for Efficient Local Search

```cpp
// Core arrays (all O(n) space)
bool in_solution[N];          // is vertex in current IS?
int conflict_count[N];        // # solution-neighbors for non-solution vertices
vector<int> adj[N];           // adjacency list
long long weight[N];          // vertex weights

// For ARW (1,2)-swap detection:
// tight[v] = non-solution neighbors of solution-vertex v with conflict_count = 1
// Maintained incrementally on each swap — O(deg) update cost
```

**Swap update procedure (O(deg)):**
```
When adding vertex u to solution:
    in_solution[u] = true
    for each neighbor w of u:
        conflict_count[w]++
        // w is now blocked if conflict_count[w] becomes 1 (now tight to u)

When removing vertex v from solution:
    in_solution[v] = false
    for each neighbor w of v:
        conflict_count[w]--
        // w becomes free if conflict_count[w] drops to 0
```

---

## 5. Complete Pipeline for N=200,000 (5-minute budget)

```
┌─────────────────────────────────────────────────────────────┐
│ Phase 1: Preprocessing (0–10s)                              │
│   1. Read input                                             │
│   2. Decompose into connected components                     │
│   3. Apply kernelization rules (deg-0, deg-1, dominance)    │
│   4. Handle trivial structures (trees, isolated vertices)   │
└────────────────────┬────────────────────────────────────────┘
                     │
┌────────────────────▼────────────────────────────────────────┐
│ Phase 2: Construction (10–30s)                              │
│   1. GWMIN greedy on remaining kernel                       │
│   2. Make solution maximal (add all free vertices)          │
│   3. Apply full ARW local search until (2,3)-local optimum  │
└────────────────────┬────────────────────────────────────────┘
                     │
┌────────────────────▼────────────────────────────────────────┐
│ Phase 3: ILS Loop (30s–280s)                                │
│   1. Adaptive perturbation (remove k vertices)              │
│   2. Greedy extension                                       │
│   3. ARW local search                                       │
│   4. Accept if better (or equal)                            │
│   5. Track global best                                      │
└────────────────────┬────────────────────────────────────────┘
                     │
┌────────────────────▼────────────────────────────────────────┐
│ Phase 4: Output (at 290s or earlier)                        │
│   1. Reconstruct solution indices from kernelization        │
│   2. Sort indices ascending                                 │
│   3. Print total weight and indices                         │
└─────────────────────────────────────────────────────────────┘
```

---

## 6. Key Repositories

| Repository | Algorithm | Notes |
|---|---|---|
| [KarlsruheMIS/KaMIS](https://github.com/KarlsruheMIS/KaMIS) | ReduMIS, MMWIS, exact | State-of-the-art C++; MIT license |
| [KennethLangedal/CHILS](https://github.com/KennethLangedal/CHILS) | Concurrent ILS | 2025 best-in-class |
| [fontanf/stablesolver](https://github.com/fontanf/stablesolver) | GWMIN, row-weighting LS | Clean modular C++ |
| [MaxiBoether/mis-benchmark-framework](https://github.com/MaxiBoether/mis-benchmark-framework) | All major solvers | ICLR 2022 benchmarks |
| [KarlsruheMIS/pace-2019](https://github.com/KarlsruheMIS/pace-2019) | Portfolio (PACE winner) | WeGotYouCovered |

---

## 7. Key Papers (Chronological)

| Year | Paper | Contribution |
|---|---|---|
| 1975 | Nemhauser & Trotter | LP half-integrality property for MWIS |
| 2001 | Sakai et al. | GWMIN/GWMAX greedy with approximation guarantees |
| 2012 | Andrade, Resende, Werneck | Fast O(m) (1,2)-swap and (2,3)-swap local search |
| 2017 | Lamm et al. | ReduMIS: finding near-optimal IS at scale |
| 2019 | Hespe et al. | Scalable kernelization (ACM JEA) |
| 2019 | Lamm et al. | Exactly solving MWIS on large real-world graphs |
| 2021 | Gellner et al. | Struction: increasing transformations (arXiv:2008.05180) |
| 2022 | Langedal et al. | Local search for large MWIS (ESA 2022) |
| 2022 | Dong & Goldberg | METAMIS: GRASP-based for 100M-node graphs |
| 2023 | Großmann et al. | MMWIS memetic algorithm — best on 205/207 benchmarks |
| 2025 | Langedal | CHILS: concurrent ILS — current state of the art |
| 2024 | arXiv:2412.09303 | Comprehensive survey of all MWIS reduction rules |

---

## 8. Benchmark Performance

On large sparse networks (real-world, N > 100k):
- Kernelization alone: reduces 90–99% of the graph
- Row-weighting local search: ~99.3% quality in under 2 seconds (192k-node caidaRouterLevel graph)
- MMWIS/CHILS: near-optimal for most instances within 300 seconds
- METAMIS: state-of-the-art on vehicle-routing instances (100M+ nodes)

On hard dense instances (BHOSLIB):
- Exact solvers time out even for 100-vertex instances
- Best heuristics (ILS, SA): typically within 1–2% of optimal
- CHILS strictly outperforms ReduMIS and METAMIS on largest benchmarks

---

## 9. Our Implementation Strategy (solution.cpp)

1. **Kernelization:** Degree-0, degree-1, degree-2 fold rules applied iteratively
2. **Greedy:** GWMIN with w(v)/(deg(v)+1) scoring
3. **Local Search:** ARW-style (1,2)-swaps with conflict_count[] and incremental updates
4. **ILS:** Adaptive perturbation loop with restart logic
5. **Timer:** Checked before each ILS iteration; terminates at 290 seconds (10s safety margin)
6. **Output:** Sorted 1-indexed coder IDs, total weight on first line

Time complexity per ILS iteration: O(m) amortized
Space complexity: O(n + m)
