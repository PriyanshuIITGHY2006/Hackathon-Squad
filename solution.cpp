/*
 * HACKATHON SQUAD — Maximum Weight Independent Set Solver
 *
 * Algorithm Pipeline:
 *   1. Kernelization    — Work-queue based; degree-0/1/2/triangle + dominance rules
 *   2. Component Split  — Each connected component solved independently
 *   3. Tree DP          — Exact O(n) for any tree/forest component
 *   4. GWMIN Greedy     — w(v)/(deg(v)+1) initial solution for hard components
 *   5. ARW Local Search — O(m) (1,2)-swaps via conf[]; sorted adj for O(log d) edge check
 *   6. ILS Loop         — Adaptive perturbation + randomized greedy diversity until 290s
 *
 * Compile: g++ -O2 -std=c++17 -o solution solution.cpp
 * Run:     ./solution < input.txt
 */

#include <bits/stdc++.h>
#include <csignal>
using namespace std;

// ─────────────────────────────────────────────────────────────────────────────
// TIMER
// ─────────────────────────────────────────────────────────────────────────────
static auto START_TIME = chrono::steady_clock::now();

inline double elapsed_sec() {
    return chrono::duration<double>(chrono::steady_clock::now() - START_TIME).count();
}

static const double TIME_LIMIT = 290.0;  // 10s safety margin before 5-minute wall

inline bool time_ok() { return elapsed_sec() < TIME_LIMIT; }

// ─────────────────────────────────────────────────────────────────────────────
// GLOBAL STATE
// ─────────────────────────────────────────────────────────────────────────────
static const int MAXN = 200005;

int N, M;
long long W[MAXN];          // vertex weights (1-indexed)
vector<int> adj[MAXN];      // adjacency list — kept SORTED for O(log d) edge queries

// Improvement 5: binary search edge query using sorted adj lists
inline bool has_edge(int u, int v) {
    // adj[u] is sorted — use binary search
    return binary_search(adj[u].begin(), adj[u].end(), v);
}

// ─────────────────────────────────────────────────────────────────────────────
// KERNELIZATION STATE
// ─────────────────────────────────────────────────────────────────────────────
bool removed[MAXN];
bool forced_in[MAXN];

inline void remove_vertex(int v) { removed[v] = true; }

// Include v in solution: remove v and all its active neighbors
void include_vertex(int v) {
    forced_in[v] = true;
    removed[v]   = true;
    for (int u : adj[v])
        if (!removed[u]) removed[u] = true;
}

// Effective degree: count active (non-removed) neighbors
inline int eff_deg(int v) {
    int d = 0;
    for (int u : adj[v]) if (!removed[u]) d++;
    return d;
}

// Get the single active neighbor of a degree-1 vertex
inline int single_neighbor(int v) {
    for (int u : adj[v]) if (!removed[u]) return u;
    return -1;
}

// ─────────────────────────────────────────────────────────────────────────────
// KERNELIZATION — work-queue based, no full rescan
// Improvement 3: degree-2 triangle + safe-inclusion rules
// Improvement 4: work queue (only re-examine vertices whose neighborhood changed)
// ─────────────────────────────────────────────────────────────────────────────
vector<int> kernelize() {
    // Seed the queue with all active vertices
    queue<int> Q;
    vector<bool> in_queue(N + 1, false);
    for (int v = 1; v <= N; v++) {
        Q.push(v);
        in_queue[v] = true;
    }

    auto enqueue_neighbors = [&](int v) {
        // When v is removed, its neighbors' effective degrees change — re-examine them
        for (int u : adj[v]) {
            if (!removed[u] && !in_queue[u]) {
                Q.push(u);
                in_queue[u] = true;
            }
        }
    };

    while (!Q.empty() && time_ok()) {
        int v = Q.front(); Q.pop();
        in_queue[v] = false;

        if (removed[v]) continue;

        int d = eff_deg(v);

        // ── Rule: Degree-0 — isolated vertex, always include ──────────────
        if (d == 0) {
            enqueue_neighbors(v); // (no active neighbors, but safe to call)
            include_vertex(v);
            continue;
        }

        // ── Rule: Degree-1 — include leaf if w(leaf) >= w(neighbor) ───────
        // Proof: if optimal S has the neighbor u but not v, swapping u→v gives
        // weight change W[v]-W[u] >= 0 → equally good or better with v.
        // When W[v] < W[u] we CANNOT safely include u (it may block high-value
        // vertices on the other side), so we skip and let heuristic decide.
        if (d == 1) {
            int u = single_neighbor(v);
            if (W[v] >= W[u]) {
                enqueue_neighbors(v);
                enqueue_neighbors(u);
                include_vertex(v);
            }
            continue;
        }

        // ── Rule: Degree-2 ─────────────────────────────────────────────────
        if (d == 2) {
            // Find the two active neighbors
            int a = -1, b = -1;
            for (int u : adj[v]) {
                if (!removed[u]) {
                    if (a == -1) a = u;
                    else { b = u; break; }
                }
            }

            // Case A: triangle (a-b edge exists)
            // At most one of {v,a,b} can be in IS — pick the heaviest
            if (has_edge(a, b)) {
                long long best = max({W[v], W[a], W[b]});
                int winner = (W[v] == best) ? v : (W[a] == best) ? a : b;
                enqueue_neighbors(v);
                enqueue_neighbors(a);
                enqueue_neighbors(b);
                // Remove the other two losers without including them
                // Then include the winner
                include_vertex(winner);
                continue;
            }

            // Case B: path a-v-b, no a-b edge
            // Safe to include v if W[v] >= W[a] + W[b]
            if (W[v] >= W[a] + W[b]) {
                enqueue_neighbors(v);
                enqueue_neighbors(a);
                enqueue_neighbors(b);
                include_vertex(v);
                continue;
            }
            // Otherwise: fold would be needed for exact reduction (complex).
            // Skip — heuristic handles it.
        }

        // ── Rule: Dominance (for low-degree vertices) ─────────────────────
        // Remove v if a neighbor u satisfies: W[u] >= W[v] AND N[v] ⊆ N[u].
        // N[x] = closed neighborhood = x + neighbors of x.
        // Intuition: u is heavier AND guards everything v guards — v is useless.
        if (d <= 12) {
            bool dominated = false;
            for (int u : adj[v]) {
                if (removed[u] || W[u] < W[v]) continue;
                // Check N[v] ⊆ N[u]: every active neighbor of v (except u itself)
                // must also be a neighbor of u. Use sorted adj + binary search.
                bool dom = true;
                for (int w : adj[v]) {
                    if (removed[w] || w == u) continue;
                    if (!has_edge(u, w)) { dom = false; break; }
                }
                if (dom) {
                    // v is dominated by u
                    enqueue_neighbors(v);
                    remove_vertex(v);
                    dominated = true;
                    break;
                }
            }
            if (dominated) continue;
        }
    }

    vector<int> kernel;
    for (int v = 1; v <= N; v++) if (!removed[v]) kernel.push_back(v);
    return kernel;
}

// ─────────────────────────────────────────────────────────────────────────────
// GLOBAL SOLUTION — written continuously so signal handler always has valid output
// ─────────────────────────────────────────────────────────────────────────────
long long    final_weight_global = 0;
vector<int>  final_sol_global;
volatile bool output_done = false;

// ─────────────────────────────────────────────────────────────────────────────
// LOCAL SEARCH STATE
// ─────────────────────────────────────────────────────────────────────────────
bool in_sol[MAXN];
int  conf[MAXN];        // conf[v] = # solution-neighbors of v
long long cur_weight;

inline void add_to_sol(int v) {
    in_sol[v]    = true;
    cur_weight  += W[v];
    for (int u : adj[v]) if (!removed[u]) conf[u]++;
}

inline void remove_from_sol(int v) {
    in_sol[v]    = false;
    cur_weight  -= W[v];
    for (int u : adj[v]) if (!removed[u]) conf[u]--;
}

inline bool is_free(int v) {
    return !in_sol[v] && !removed[v] && conf[v] == 0;
}

void reset_sol(const vector<int>& comp) {
    for (int v : comp) { in_sol[v] = false; conf[v] = 0; }
    cur_weight = 0;
}

// ─────────────────────────────────────────────────────────────────────────────
// GREEDY CONSTRUCTION
// Improvement 6: accept an rng seed → randomized greedy for ILS diversity
// ─────────────────────────────────────────────────────────────────────────────
// randomness = 0.0 → pure GWMIN (deterministic)
// randomness > 0.0 → GWMIN score perturbed by random noise (exploration)
void greedy_build(const vector<int>& comp, mt19937& rng, double randomness = 0.0) {
    reset_sol(comp);

    vector<pair<double, int>> scored;
    scored.reserve(comp.size());

    uniform_real_distribution<double> noise(1.0 - randomness, 1.0 + randomness);

    for (int v : comp) {
        int d = eff_deg(v);
        double score = (double)W[v] / (d + 1);
        if (randomness > 0.0) score *= noise(rng);
        scored.push_back({score, v});
    }
    sort(scored.rbegin(), scored.rend());

    for (auto& [sc, v] : scored) {
        if (!removed[v] && !in_sol[v] && conf[v] == 0)
            add_to_sol(v);
    }
}

void make_maximal(const vector<int>& comp) {
    for (int v : comp)
        if (is_free(v)) add_to_sol(v);
}

// ─────────────────────────────────────────────────────────────────────────────
// ARW LOCAL SEARCH — (1,2)-swaps and (2,3)-swaps
// Improvement 5: edge query via binary search on sorted adj
// ─────────────────────────────────────────────────────────────────────────────
bool one_two_swap_pass(const vector<int>& comp) {
    bool improved = false;

    for (int v : comp) {
        if (!in_sol[v]) continue;

        // Collect 1-tight neighbors: non-solution vertices whose only
        // solution-neighbor is v (conf[u]==1). They can enter the moment v leaves.
        vector<int> tight;
        for (int u : adj[v]) {
            if (!removed[u] && !in_sol[u] && conf[u] == 1)
                tight.push_back(u);
        }
        if ((int)tight.size() < 2) continue;

        // Try all pairs (u1, u2): valid if no edge u1-u2 AND gain > 0
        for (int i = 0; i < (int)tight.size(); i++) {
            for (int j = i + 1; j < (int)tight.size(); j++) {
                int u1 = tight[i], u2 = tight[j];
                if (has_edge(u1, u2)) continue;            // O(log d) binary search
                if (W[u1] + W[u2] > W[v]) {
                    remove_from_sol(v);
                    add_to_sol(u1);
                    add_to_sol(u2);
                    improved = true;
                    goto next_v;
                }
            }
        }
        next_v:;
    }
    return improved;
}

// (2,3)-swap: remove 2 solution vertices, add 3 non-solution vertices
// Restricted to low-degree vertices for tractability
bool two_three_swap_pass(const vector<int>& comp) {
    bool improved = false;

    for (int i = 0; i < (int)comp.size() && time_ok(); i++) {
        int v1 = comp[i];
        if (!in_sol[v1] || (int)adj[v1].size() > 25) continue;

        for (int j = i + 1; j < (int)comp.size(); j++) {
            int v2 = comp[j];
            if (!in_sol[v2] || (int)adj[v2].size() > 25) continue;

            remove_from_sol(v1);
            remove_from_sol(v2);

            // Candidates: free vertices in N(v1) ∪ N(v2)
            vector<int> cands;
            for (int u : adj[v1])
                if (!removed[u] && !in_sol[u] && conf[u] == 0)
                    cands.push_back(u);
            for (int u : adj[v2])
                if (!removed[u] && !in_sol[u] && conf[u] == 0)
                    cands.push_back(u);

            // Deduplicate and sort by weight descending
            sort(cands.begin(), cands.end());
            cands.erase(unique(cands.begin(), cands.end()), cands.end());
            sort(cands.begin(), cands.end(), [](int a, int b){ return W[a] > W[b]; });

            int lim = min((int)cands.size(), 15);
            bool found = false;
            for (int p = 0; p < lim && !found; p++) {
                int u1 = cands[p];
                add_to_sol(u1);
                for (int q = p + 1; q < lim && !found; q++) {
                    int u2 = cands[q];
                    if (conf[u2] > 0) continue;
                    add_to_sol(u2);
                    for (int r = q + 1; r < lim && !found; r++) {
                        int u3 = cands[r];
                        if (conf[u3] > 0) continue;
                        if (W[u1] + W[u2] + W[u3] > W[v1] + W[v2]) {
                            add_to_sol(u3);
                            found = true;
                            improved = true;
                        }
                    }
                    if (!found) remove_from_sol(u2);
                }
                if (!found) remove_from_sol(u1);
            }

            if (!found) {
                add_to_sol(v1);
                add_to_sol(v2);
            } else {
                goto next_v1;
            }
        }
        next_v1:;
    }
    return improved;
}

void local_search(const vector<int>& comp) {
    make_maximal(comp);
    bool improved = true;
    while (improved && time_ok()) {
        improved = false;
        if (one_two_swap_pass(comp))            improved = true;
        if (time_ok() && two_three_swap_pass(comp)) improved = true;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// TREE DP — exact O(n) solution for tree/forest components
// ─────────────────────────────────────────────────────────────────────────────
long long dp_in[MAXN], dp_out[MAXN];
int par[MAXN];

// Iterative post-order DP (avoids stack overflow on deep trees)
void tree_dp_iterative(const vector<int>& comp) {
    int root = comp[0];
    vector<int> order;
    order.reserve(comp.size());
    // BFS to assign parents and get processing order
    vector<bool> vis(N + 1, false);
    queue<int> bfsq;
    bfsq.push(root); vis[root] = true; par[root] = -1;
    while (!bfsq.empty()) {
        int v = bfsq.front(); bfsq.pop();
        order.push_back(v);
        for (int u : adj[v]) {
            if (!removed[u] && !vis[u]) {
                vis[u] = true; par[u] = v;
                bfsq.push(u);
            }
        }
    }
    // Process leaves → root
    for (int i = (int)order.size() - 1; i >= 0; i--) {
        int v = order[i];
        dp_in[v]  = W[v];
        dp_out[v] = 0;
        for (int u : adj[v]) {
            if (!removed[u] && u != par[v]) {
                dp_in[v]  += dp_out[u];
                dp_out[v] += max(dp_in[u], dp_out[u]);
            }
        }
    }
}

// Iterative reconstruction
void tree_reconstruct(int root, bool take_root, vector<int>& result) {
    queue<pair<int,bool>> q;
    q.push({root, take_root});
    while (!q.empty()) {
        auto [v, take] = q.front(); q.pop();
        if (take) {
            result.push_back(v);
            for (int u : adj[v])
                if (!removed[u] && u != par[v]) q.push({u, false});
        } else {
            for (int u : adj[v])
                if (!removed[u] && u != par[v]) q.push({u, dp_in[u] >= dp_out[u]});
        }
    }
}

// Solve one component exactly if it is a tree/forest
// Returns true + fills result if solved; false otherwise
bool solve_as_tree(const vector<int>& comp, vector<int>& result, long long& score) {
    // Forest: edges = nodes - #components. Quick reject if too many edges.
    long long edges = 0;
    for (int v : comp)
        for (int u : adj[v]) if (!removed[u] && u > v) edges++;
    if (edges >= (long long)comp.size()) return false;

    // Union-find cycle detection
    unordered_map<int,int> uf;
    for (int v : comp) uf[v] = v;
    function<int(int)> find = [&](int x) -> int {
        return uf[x] == x ? x : uf[x] = find(uf[x]);
    };
    for (int v : comp) {
        for (int u : adj[v]) {
            if (removed[u] || u <= v) continue;
            int rv = find(v), ru = find(u);
            if (rv == ru) return false;   // cycle → not a forest
            uf[rv] = ru;
        }
    }

    // It is a forest — solve each sub-component with tree DP
    unordered_set<int> incomp(comp.begin(), comp.end());
    vector<bool> vis(N + 1, false);
    score = 0;

    for (int root : comp) {
        if (vis[root]) continue;
        // BFS to find this sub-component
        vector<int> sub;
        queue<int> q;
        q.push(root); vis[root] = true;
        while (!q.empty()) {
            int v = q.front(); q.pop();
            sub.push_back(v);
            for (int u : adj[v])
                if (!removed[u] && !vis[u] && incomp.count(u)) {
                    vis[u] = true; q.push(u);
                }
        }
        tree_dp_iterative(sub);
        int r = sub[0];
        score += max(dp_in[r], dp_out[r]);
        tree_reconstruct(r, dp_in[r] >= dp_out[r], result);
    }
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// ILS — per component
// Improvement 6: randomized greedy constructions for diversity
// ─────────────────────────────────────────────────────────────────────────────
void perturb_sol(const vector<int>& comp, int k, mt19937& rng) {
    vector<int> sv;
    for (int v : comp) if (in_sol[v]) sv.push_back(v);
    shuffle(sv.begin(), sv.end(), rng);
    k = min(k, (int)sv.size());
    for (int i = 0; i < k; i++) remove_from_sol(sv[i]);
}

// Run ILS on a single hard component; returns (best_weight, best_vertices)
// Also writes into final_sol_global / final_weight_global continuously so
// the signal handler can always print a valid partial solution if killed early.
pair<long long, vector<int>> ils_component(const vector<int>& comp, double time_share,
                                           long long base_weight,
                                           const vector<int>& base_sol) {
    mt19937 rng(chrono::steady_clock::now().time_since_epoch().count());

    double deadline = elapsed_sec() + time_share;
    auto within = [&]() { return elapsed_sec() < min(deadline, TIME_LIMIT); };

    long long  best_w = 0;
    vector<int> best_v;

    auto update_best = [&]() {
        if (cur_weight > best_w) {
            best_w = cur_weight;
            best_v.clear();
            for (int v : comp) if (in_sol[v]) best_v.push_back(v);

            // Keep global solution up-to-date for signal handler
            final_weight_global = base_weight + best_w;
            final_sol_global    = base_sol;
            for (int v : best_v) final_sol_global.push_back(v);
        }
    };

    auto restore_best = [&]() {
        reset_sol(comp);
        for (int v : best_v) add_to_sol(v);
    };

    // ── Initial solution: deterministic GWMIN ─────────────────────────────
    greedy_build(comp, rng, 0.0);
    local_search(comp);
    update_best();

    double perturb_rate = 0.10;
    int no_improve = 0;
    int iteration  = 0;

    while (within()) {
        restore_best();

        // Every 5th iteration: try a fresh randomized greedy (diversification)
        // This escapes regions the ILS perturbation can't reach
        if (iteration % 5 == 4) {
            greedy_build(comp, rng, 0.25);   // 25% noise on scores
        } else {
            int k = max(3, (int)(best_v.size() * perturb_rate));
            perturb_sol(comp, k, rng);
        }

        make_maximal(comp);
        local_search(comp);

        if (cur_weight >= best_w) {
            update_best();
            no_improve    = 0;
            perturb_rate  = 0.10;
        } else {
            no_improve++;
            if (no_improve > 40) {
                perturb_rate = min(0.35, perturb_rate * 1.2);
                no_improve   = 0;
            }
        }
        iteration++;
    }

    return {best_w, best_v};
}

// ─────────────────────────────────────────────────────────────────────────────
// CONNECTED COMPONENT DECOMPOSITION
// Improvement 1 + 2: split kernel into components, apply tree DP per component
// ─────────────────────────────────────────────────────────────────────────────
vector<vector<int>> get_components(const vector<int>& kernel) {
    unordered_set<int> kset(kernel.begin(), kernel.end());
    vector<bool> vis(N + 1, false);
    vector<vector<int>> components;

    for (int root : kernel) {
        if (vis[root]) continue;
        vector<int> comp;
        queue<int> q;
        q.push(root); vis[root] = true;
        while (!q.empty()) {
            int v = q.front(); q.pop();
            comp.push_back(v);
            for (int u : adj[v])
                if (!removed[u] && !vis[u] && kset.count(u)) {
                    vis[u] = true; q.push(u);
                }
        }
        components.push_back(move(comp));
    }
    return components;
}

// ─────────────────────────────────────────────────────────────────────────────
// SIGNAL HANDLER — print best solution even if judge kills the process early
// ─────────────────────────────────────────────────────────────────────────────
void print_solution() {
    if (output_done) return;
    output_done = true;
    sort(final_sol_global.begin(), final_sol_global.end());
    final_sol_global.erase(unique(final_sol_global.begin(), final_sol_global.end()),
                           final_sol_global.end());
    cout << final_weight_global << "\n";
    for (int i = 0; i < (int)final_sol_global.size(); i++) {
        if (i) cout << ' ';
        cout << final_sol_global[i];
    }
    cout << "\n";
    cout.flush();
}

void signal_handler(int) { print_solution(); _exit(0); }

// ─────────────────────────────────────────────────────────────────────────────
// MAIN
// ─────────────────────────────────────────────────────────────────────────────
int main() {
    ios::sync_with_stdio(false);
    cin.tie(nullptr);

    // Register signal handlers — output best solution on any forced termination
    signal(SIGTERM, signal_handler);
    signal(SIGINT,  signal_handler);

    // ── Input ──────────────────────────────────────────────────────────────
    cin >> N >> M;
    for (int i = 1; i <= N; i++) cin >> W[i];
    for (int i = 0; i < M; i++) {
        int u, v; cin >> u >> v;
        adj[u].push_back(v);
        adj[v].push_back(u);
    }

    // Sort adjacency lists once — enables O(log d) has_edge() via binary search
    for (int i = 1; i <= N; i++) sort(adj[i].begin(), adj[i].end());

    // ── Kernelization ──────────────────────────────────────────────────────
    memset(removed,   false, sizeof(removed));
    memset(forced_in, false, sizeof(forced_in));
    memset(in_sol,    false, sizeof(in_sol));
    memset(conf,      0,     sizeof(conf));
    cur_weight = 0;

    vector<int> kernel = kernelize();

    // Collect the weight already decided by kernelization
    // Use globals so signal handler can always print the best found so far
    for (int v = 1; v <= N; v++) {
        if (forced_in[v]) {
            final_weight_global += W[v];
            final_sol_global.push_back(v);
        }
    }

    if (kernel.empty()) goto output;

    // ── Split kernel into connected components ─────────────────────────────
    {
        auto components = get_components(kernel);

        // Sort components: larger ones last (they get more ILS time)
        sort(components.begin(), components.end(),
             [](const vector<int>& a, const vector<int>& b){ return a.size() < b.size(); });

        // Count total "hard" (non-tree) component nodes for time allocation
        long long total_hard_nodes = 0;
        vector<bool> is_hard(components.size(), false);
        vector<pair<long long,vector<int>>> tree_results(components.size());

        for (int ci = 0; ci < (int)components.size(); ci++) {
            auto& comp = components[ci];
            long long sc = 0;
            vector<int> res;
            if (solve_as_tree(comp, res, sc)) {
                // Exact tree DP result
                tree_results[ci] = {sc, res};
                is_hard[ci] = false;
            } else {
                is_hard[ci] = true;
                total_hard_nodes += comp.size();
            }
        }

        // Collect tree DP results
        for (int ci = 0; ci < (int)components.size(); ci++) {
            if (!is_hard[ci]) {
                final_weight_global += tree_results[ci].first;
                for (int v : tree_results[ci].second) final_sol_global.push_back(v);
            }
        }

        // ILS for hard components — allocate time proportional to component size
        double time_remaining = TIME_LIMIT - elapsed_sec();

        for (int ci = 0; ci < (int)components.size() && time_ok(); ci++) {
            if (!is_hard[ci]) continue;
            auto& comp = components[ci];

            double share = (total_hard_nodes > 0)
                           ? time_remaining * (double)comp.size() / total_hard_nodes
                           : time_remaining;

            // Give at least 1s to tiny components, cap massive ones
            share = max(1.0, share);

            // Pass current global state as base so signal handler always
            // has a valid combined solution (base + this component's best so far)
            auto [w, verts] = ils_component(comp, share,
                                            final_weight_global,
                                            final_sol_global);
            final_weight_global += w;
            for (int v : verts) final_sol_global.push_back(v);
        }
    }

output:
    print_solution();
    return 0;
}
