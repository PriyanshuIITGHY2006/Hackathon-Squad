
#include <bits/stdc++.h>
#include <csignal>
using namespace std;

static auto START_TIME = chrono::steady_clock::now();

inline double elapsed_sec() {
    return chrono::duration<double>(chrono::steady_clock::now() - START_TIME).count();
}

static const double TIME_LIMIT = 290.0;

inline bool time_ok() { return elapsed_sec() < TIME_LIMIT; }

static const int MAXN = 200005;

int N, M;
long long W[MAXN];
vector<int> adj[MAXN];

inline bool has_edge(int u, int v) {
    return binary_search(adj[u].begin(), adj[u].end(), v);
}

bool removed[MAXN];
bool forced_in[MAXN];
int  live_deg[MAXN];   // maintained effective degree: O(1) lookup

struct FoldRecord { int type, v, a, b; };  // type 1=deg1-fold, 2=deg2-fold
vector<FoldRecord> fold_records;
long long fold_offset = 0;

// Mark v removed and decrement live_deg of active neighbors
inline void mark_removed(int v) {
    removed[v] = true;
    for (int u : adj[v]) if (!removed[u]) live_deg[u]--;
}

// Dominance-remove v (not included in solution)
inline void remove_vertex(int v) { mark_removed(v); }

// Include v in solution: fix v in, remove v and all active neighbors
void include_vertex(int v) {
    forced_in[v] = true;
    // Collect active neighbors before we start removing (to avoid double-decrement)
    vector<int> nbrs;
    for (int u : adj[v]) if (!removed[u]) nbrs.push_back(u);
    mark_removed(v);
    for (int u : nbrs) if (!removed[u]) mark_removed(u);
}

inline int single_neighbor(int v) {
    for (int u : adj[v]) if (!removed[u]) return u;
    return -1;
}

// ── Dinic max-flow (used by LP/NT reduction) ─────────────────────────────────
struct Dinic {
    struct Edge { int to, rev; long long cap; };
    int n;
    vector<vector<Edge>> g;
    vector<int> level, cur;
    explicit Dinic(int n) : n(n), g(n), level(n), cur(n) {}
    void add_edge(int u, int v, long long cap) {
        g[u].push_back({v,(int)g[v].size(),cap});
        g[v].push_back({u,(int)g[u].size()-1,0LL});
    }
    bool bfs(int s, int t) {
        fill(level.begin(),level.end(),-1);
        queue<int> q; level[s]=0; q.push(s);
        while (!q.empty()) {
            int v=q.front(); q.pop();
            for (auto& e:g[v]) if (e.cap>0&&level[e.to]<0) { level[e.to]=level[v]+1; q.push(e.to); }
        }
        return level[t]>=0;
    }
    long long dfs(int v, int t, long long f) {
        if (v==t) return f;
        for (int& i=cur[v]; i<(int)g[v].size(); i++) {
            Edge& e=g[v][i];
            if (e.cap<=0||level[e.to]!=level[v]+1) continue;
            long long d=dfs(e.to,t,min(f,e.cap));
            if (d>0) { e.cap-=d; g[e.to][e.rev].cap+=d; return d; }
        }
        return 0;
    }
    long long max_flow(int s, int t) {
        long long flow=0;
        while (bfs(s,t)) { fill(cur.begin(),cur.end(),0); long long d; while ((d=dfs(s,t,(long long)4e18))>0) flow+=d; }
        return flow;
    }
    vector<bool> reachable(int s) {
        vector<bool> vis(n,false); queue<int> q; vis[s]=true; q.push(s);
        while (!q.empty()) { int v=q.front(); q.pop(); for (auto& e:g[v]) if (e.cap>0&&!vis[e.to]) { vis[e.to]=true; q.push(e.to); } }
        return vis;
    }
};

// LP (Nemhauser-Trotter) reduction: forces LP=1 vertices in, LP=0 vertices out.
// Returns number of vertices decided.  Re-enqueues affected nodes for basic rules.
static int lp_reduce(queue<int>& Q, vector<bool>& inQ) {
    const long long INF = (long long)4e18;
    const int S = 0, T = 2*N+1;
    Dinic din(2*N+2);
    for (int v = 1; v <= N; v++) {
        if (removed[v]) continue;
        din.add_edge(S,   v,   W[v]);
        din.add_edge(N+v, T,   W[v]);
    }
    for (int v = 1; v <= N; v++) {
        if (removed[v]) continue;
        for (int u : adj[v]) {
            if (removed[u] || u <= v) continue;
            din.add_edge(v,   N+u, INF);
            din.add_edge(u,   N+v, INF);
        }
    }
    din.max_flow(S, T);
    auto R = din.reachable(S);  // R[node] = reachable in residual from s

    auto enq = [&](int u) { if (!removed[u]&&!inQ[u]) { Q.push(u); inQ[u]=true; } };

    int decided = 0;
    vector<int> inc, exc;
    for (int v = 1; v <= N; v++) {
        if (removed[v]) continue;
        bool lv = R[v], rv = R[N+v];
        if (lv && !rv)  inc.push_back(v);   // LP IS=1 → force in
        if (!lv && rv)  exc.push_back(v);   // LP IS=0 → force out
    }
    for (int v : exc) if (!removed[v]) { for (int u:adj[v]) enq(u); mark_removed(v); decided++; }
    for (int v : inc) if (!removed[v]) { for (int u:adj[v]) enq(u); include_vertex(v); decided++; }
    return decided;
}

vector<int> kernelize() {
    queue<int> Q;
    vector<bool> in_queue(N + 1, false);
    for (int v = 1; v <= N; v++) {
        Q.push(v);
        in_queue[v] = true;
    }

    auto enqueue_neighbors = [&](int v) {
        for (int u : adj[v]) {
            if (!removed[u] && !in_queue[u]) {
                Q.push(u);
                in_queue[u] = true;
            }
        }
    };

    // Basic reductions (deg-0/1/2 + dominance)
    while (!Q.empty() && time_ok()) {
        int v = Q.front(); Q.pop();
        in_queue[v] = false;
        if (removed[v]) continue;

        int d = live_deg[v];

        // ── Degree-0: isolated vertex, always include ─────────────────────
        if (d == 0) {
            include_vertex(v);
            continue;
        }

        // ── Degree-1 ───────────────────────────────────────────────────────
        if (d == 1) {
            int u = single_neighbor(v);
            if (W[v] >= W[u]) {
                enqueue_neighbors(u);
                include_vertex(v);
            } else {
                // N-fold: W[v] < W[u]; fold leaf v away
                // opt({v,u}) = W[v] + opt_kernel(u'), where W[u'] = W[u]-W[v]
                fold_records.push_back({1, v, u, -1});
                fold_offset += W[v];
                W[u] -= W[v];
                enqueue_neighbors(v);
                if (!in_queue[u]) { Q.push(u); in_queue[u] = true; }
                mark_removed(v);
            }
            continue;
        }

        // ── Degree-2 ───────────────────────────────────────────────────────
        if (d == 2) {
            int a = -1, b = -1;
            for (int u : adj[v]) {
                if (!removed[u]) {
                    if (a == -1) a = u;
                    else { b = u; break; }
                }
            }
            // Triangle: at most one of {v,a,b} in IS — pick the heaviest
            if (has_edge(a, b)) {
                long long best = max({W[v], W[a], W[b]});
                int winner = (W[v] == best) ? v : (W[a] == best) ? a : b;
                enqueue_neighbors(winner);
                for (int x : {v, a, b}) enqueue_neighbors(x);
                include_vertex(winner);
                continue;
            }
            // Path a-v-b: include v if W[v] >= W[a]+W[b]
            if (W[v] >= W[a] + W[b]) {
                enqueue_neighbors(v);
                enqueue_neighbors(a);
                enqueue_neighbors(b);
                include_vertex(v);
                continue;
            }
            // W[v] < W[a]+W[b]: leave for LP reduction; V-fold only valid post-LP
            continue;
        }

        // ── Dominance: remove v if a heavier neighbor u has N[v] ⊆ N[u] ──
        if (d <= 12) {
            bool dominated = false;
            for (int u : adj[v]) {
                if (removed[u] || W[u] < W[v]) continue;
                bool dom = true;
                for (int w : adj[v]) {
                    if (removed[w] || w == u) continue;
                    if (!has_edge(u, w)) { dom = false; break; }
                }
                if (dom) {
                    enqueue_neighbors(v);
                    remove_vertex(v);
                    dominated = true;
                    break;
                }
            }
            if (dominated) continue;
        }
    }

    // LP (Nemhauser-Trotter) reduction: forces LP=0/1 vertices out/in
    if (time_ok()) {
        int decided = lp_reduce(Q, in_queue);
        if (decided > 0) {
            // Run basic reductions on the LP-modified graph
            while (!Q.empty() && time_ok()) {
                int v = Q.front(); Q.pop();
                in_queue[v] = false;
                if (removed[v]) continue;
                int d = live_deg[v];
                if (d == 0) { include_vertex(v); continue; }
                if (d == 1) {
                    int u = single_neighbor(v);
                    if (W[v] >= W[u]) { enqueue_neighbors(u); include_vertex(v); }
                    else {
                        fold_records.push_back({1,v,u,-1}); fold_offset+=W[v]; W[u]-=W[v];
                        enqueue_neighbors(v); if (!in_queue[u]) { Q.push(u); in_queue[u]=true; }
                        mark_removed(v);
                    }
                    continue;
                }
                if (d == 2) {
                    int a=-1,b=-1;
                    for (int u:adj[v]) if (!removed[u]) { if (a<0) a=u; else b=u; }
                    if (has_edge(a,b)) {
                        long long best=max({W[v],W[a],W[b]}); int win=(W[v]==best)?v:(W[a]==best)?a:b;
                        for (int x:{v,a,b}) enqueue_neighbors(x); include_vertex(win); continue;
                    }
                    if (W[v]>=W[a]+W[b]) { enqueue_neighbors(v); enqueue_neighbors(a); enqueue_neighbors(b); include_vertex(v); continue; }
                    fold_records.push_back({2,v,a,b}); fold_offset+=W[v]; W[a]=W[a]+W[b]-W[v];
                    unordered_set<int> aset(adj[a].begin(),adj[a].end());
                    vector<int> tnew;
                    for (int u:adj[b]) { if (u==v||u==a||removed[u]) continue; if (!aset.count(u)) { tnew.push_back(u); aset.insert(u); live_deg[u]++; adj[u].insert(lower_bound(adj[u].begin(),adj[u].end(),a),a); } }
                    for (int u:tnew) adj[a].insert(lower_bound(adj[a].begin(),adj[a].end(),u),u);
                    enqueue_neighbors(a); mark_removed(v); mark_removed(b);
                    live_deg[a]=0; for (int u:adj[a]) if (!removed[u]) live_deg[a]++;
                    if (!in_queue[a]) { Q.push(a); in_queue[a]=true; }
                    continue;
                }
                if (d <= 12) {
                    bool dom=false;
                    for (int u:adj[v]) { if (removed[u]||W[u]<W[v]) continue; bool ok=true; for (int w:adj[v]) { if (removed[w]||w==u) continue; if (!has_edge(u,w)) { ok=false; break; } } if (ok) { enqueue_neighbors(v); mark_removed(v); dom=true; break; } }
                }
            }
        }
    }

    vector<int> kernel;
    for (int v = 1; v <= N; v++) if (!removed[v]) kernel.push_back(v);
    return kernel;
}

long long    final_weight_global = 0;
vector<int>  final_sol_global;
volatile bool output_done = false;

bool in_sol[MAXN];
int  conf[MAXN];
long long cur_weight;

inline void add_to_sol(int v) {
    in_sol[v]   = true;
    cur_weight += W[v];
    for (int u : adj[v]) if (!removed[u]) conf[u]++;
}

inline void remove_from_sol(int v) {
    in_sol[v]   = false;
    cur_weight -= W[v];
    for (int u : adj[v]) if (!removed[u]) conf[u]--;
}

inline bool is_free(int v) {
    return !in_sol[v] && !removed[v] && conf[v] == 0;
}

void reset_sol(const vector<int>& comp) {
    for (int v : comp) { in_sol[v] = false; conf[v] = 0; }
    cur_weight = 0;
}

void greedy_build(const vector<int>& comp, mt19937& rng, double randomness = 0.0) {
    reset_sol(comp);
    vector<pair<double, int>> scored;
    scored.reserve(comp.size());
    uniform_real_distribution<double> noise(1.0 - randomness, 1.0 + randomness);
    for (int v : comp) {
        int d = live_deg[v];
        double score = (double)W[v] / (d + 1);
        if (randomness > 0.0) score *= noise(rng);
        scored.push_back({score, v});
    }
    sort(scored.rbegin(), scored.rend());
    for (auto& [sc, v] : scored)
        if (!removed[v] && !in_sol[v] && conf[v] == 0)
            add_to_sol(v);
}

void make_maximal(const vector<int>& comp) {
    for (int v : comp)
        if (is_free(v)) add_to_sol(v);
}

bool one_two_swap_pass(const vector<int>& comp) {
    bool improved = false;
    for (int v : comp) {
        if (!in_sol[v]) continue;
        vector<int> tight;
        for (int u : adj[v])
            if (!removed[u] && !in_sol[u] && conf[u] == 1)
                tight.push_back(u);
        if ((int)tight.size() < 2) continue;
        for (int i = 0; i < (int)tight.size(); i++) {
            for (int j = i + 1; j < (int)tight.size(); j++) {
                int u1 = tight[i], u2 = tight[j];
                if (has_edge(u1, u2)) continue;
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

// Try one (2,3)-swap for a given (v1, v2) pair.
// Returns true if a swap was accepted (v1/v2 already removed from sol on entry).
// If false, v1 and v2 are restored.
inline bool try_23_pair(int v1, int v2, int cand_limit) {
    // Free vertices reachable from N(v1) ∪ N(v2) after removing both
    vector<int> cands;
    for (int u : adj[v1])
        if (!removed[u] && !in_sol[u] && conf[u] == 0) cands.push_back(u);
    for (int u : adj[v2])
        if (!removed[u] && !in_sol[u] && conf[u] == 0) cands.push_back(u);

    sort(cands.begin(), cands.end());
    cands.erase(unique(cands.begin(), cands.end()), cands.end());
    sort(cands.begin(), cands.end(), [](int a, int b){ return W[a] > W[b]; });

    int lim = min((int)cands.size(), cand_limit);
    for (int p = 0; p < lim; p++) {
        int u1 = cands[p];
        add_to_sol(u1);
        for (int q = p + 1; q < lim; q++) {
            int u2 = cands[q];
            if (conf[u2] > 0) continue;
            add_to_sol(u2);
            for (int r = q + 1; r < lim; r++) {
                int u3 = cands[r];
                if (conf[u3] > 0) continue;
                if (W[u1] + W[u2] + W[u3] > W[v1] + W[v2]) {
                    add_to_sol(u3);
                    return true;
                }
            }
            remove_from_sol(u2);
        }
        remove_from_sol(u1);
    }
    return false;
}

bool two_three_swap_pass(const vector<int>& comp) {
    bool improved = false;
    // Larger candidate window for small components — worth the extra O(lim³) cost
    int cand_limit = ((int)comp.size() <= 300) ? 25 : 15;

    vector<int> sol_vec;
    sol_vec.reserve(comp.size());
    for (int v : comp) if (in_sol[v]) sol_vec.push_back(v);

    int sz = (int)sol_vec.size();
    if (sz < 2) return false;

    for (int i = 0; i < sz && time_ok(); i++) {
        int v1 = sol_vec[i];
        if (!in_sol[v1]) continue;
        remove_from_sol(v1);
        for (int j = i + 1; j < sz && time_ok(); j++) {
            int v2 = sol_vec[j];
            if (!in_sol[v2]) continue;
            remove_from_sol(v2);
            if (try_23_pair(v1, v2, cand_limit)) { improved = true; goto next_v1; }
            add_to_sol(v2);
        }
        add_to_sol(v1);
        next_v1:;
    }
    return improved;
}

// PROBE: for each non-IS vertex u, check if W[u] > sum of IS-neighbor weights.
// If so, remove all IS-neighbors and add u — a profitable (1→k) swap.
// Handles the cases missed by the (1,2) and (2,3) passes.
bool probe_pass(const vector<int>& comp) {
    bool improved = false;
    for (int u : comp) {
        if (in_sol[u] || removed[u]) continue;
        if (conf[u] == 0) {
            add_to_sol(u);
            improved = true;
            continue;
        }
        long long gain = W[u];
        vector<int> nbrs_in_sol;
        for (int v : adj[u]) {
            if (!removed[v] && in_sol[v]) {
                gain -= W[v];
                nbrs_in_sol.push_back(v);
            }
        }
        if (gain > 0) {
            for (int v : nbrs_in_sol) remove_from_sol(v);
            add_to_sol(u);
            improved = true;
        }
    }
    return improved;
}

void local_search(const vector<int>& comp) {
    make_maximal(comp);
    bool improved = true;
    while (improved && time_ok()) {
        improved = false;
        if (probe_pass(comp))                         improved = true;
        if (time_ok() && one_two_swap_pass(comp))     improved = true;
        if (time_ok() && two_three_swap_pass(comp))   improved = true;
    }
}

long long dp_in[MAXN], dp_out[MAXN];
int par[MAXN];

void tree_dp_iterative(const vector<int>& comp) {
    int root = comp[0];
    vector<int> order;
    order.reserve(comp.size());
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

bool solve_as_tree(const vector<int>& comp, vector<int>& result, long long& score) {
    long long edges = 0;
    for (int v : comp)
        for (int u : adj[v]) if (!removed[u] && u > v) edges++;
    if (edges >= (long long)comp.size()) return false;

    unordered_map<int,int> uf;
    for (int v : comp) uf[v] = v;
    function<int(int)> find = [&](int x) -> int {
        return uf[x] == x ? x : uf[x] = find(uf[x]);
    };
    for (int v : comp) {
        for (int u : adj[v]) {
            if (removed[u] || u <= v) continue;
            int rv = find(v), ru = find(u);
            if (rv == ru) return false;
            uf[rv] = ru;
        }
    }

    unordered_set<int> incomp(comp.begin(), comp.end());
    vector<bool> vis(N + 1, false);
    score = 0;
    for (int root : comp) {
        if (vis[root]) continue;
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

void perturb_sol(const vector<int>& comp, int k, mt19937& rng) {
    vector<int> sv;
    for (int v : comp) if (in_sol[v]) sv.push_back(v);
    shuffle(sv.begin(), sv.end(), rng);
    k = min(k, (int)sv.size());
    for (int i = 0; i < k; i++) remove_from_sol(sv[i]);
}

pair<long long, vector<int>> ils_component(const vector<int>& comp, double time_share,
                                           long long base_weight,
                                           vector<int> base_sol) {  // passed by VALUE: snapshot, not aliased ref
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
            final_weight_global = base_weight + best_w;
            final_sol_global    = base_sol;
            for (int v : best_v) final_sol_global.push_back(v);
        }
    };

    auto restore_best = [&]() {
        reset_sol(comp);
        for (int v : best_v) add_to_sol(v);
    };

    greedy_build(comp, rng, 0.0);
    local_search(comp);  // runs until convergence or global TIME_LIMIT
    update_best();


    double perturb_rate = 0.10;
    int no_improve = 0;
    int iteration  = 0;

    while (within()) {
        restore_best();

        if (iteration % 5 == 4) {
            greedy_build(comp, rng, 0.25);
        } else {
            int k = max(3, (int)(best_v.size() * perturb_rate));
            perturb_sol(comp, k, rng);
        }

        make_maximal(comp);
        local_search(comp);

        if (cur_weight >= best_w) {
            update_best();
            no_improve   = 0;
            perturb_rate = 0.10;
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

static bool unfold_done = false;
void unfold_solution() {
    if (unfold_done || fold_records.empty()) return;
    unfold_done = true;
    vector<bool> in_is(N + 1, false);
    for (int v : final_sol_global) in_is[v] = true;

    for (int i = (int)fold_records.size() - 1; i >= 0; i--) {
        auto& fr = fold_records[i];
        if (fr.type == 1) {
            // deg-1 fold: if supernode a not in IS → add leaf v
            if (!in_is[fr.a]) {
                in_is[fr.v] = true;
                final_sol_global.push_back(fr.v);
            }
        } else {
            // deg-2 fold: if supernode a in IS → add b; else → add v
            if (in_is[fr.a]) {
                in_is[fr.b] = true;
                final_sol_global.push_back(fr.b);
            } else {
                in_is[fr.v] = true;
                final_sol_global.push_back(fr.v);
            }
        }
    }
    final_weight_global += fold_offset;
}

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

void signal_handler(int) { unfold_solution(); print_solution(); _exit(0); }

int main() {
    ios::sync_with_stdio(false);
    cin.tie(nullptr);

    signal(SIGTERM, signal_handler);
    signal(SIGINT,  signal_handler);

    cin >> N >> M;
    for (int i = 1; i <= N; i++) cin >> W[i];
    for (int i = 0; i < M; i++) {
        int u, v; cin >> u >> v;
        adj[u].push_back(v);
        adj[v].push_back(u);
    }

    // Sort adjacency lists for O(log d) has_edge()
    for (int i = 1; i <= N; i++) {
        sort(adj[i].begin(), adj[i].end());
        live_deg[i] = (int)adj[i].size();
    }

    memset(removed,   false, sizeof(removed));
    memset(forced_in, false, sizeof(forced_in));
    memset(in_sol,    false, sizeof(in_sol));
    memset(conf,      0,     sizeof(conf));
    cur_weight = 0;

    vector<int> kernel = kernelize();

    for (int v = 1; v <= N; v++) {
        if (forced_in[v]) {
            final_weight_global += W[v];
            final_sol_global.push_back(v);
        }
    }

    if (kernel.empty()) goto output;

    {
        auto components = get_components(kernel);

        sort(components.begin(), components.end(),
             [](const vector<int>& a, const vector<int>& b){ return a.size() < b.size(); });

        long long total_hard_nodes = 0;
        vector<bool> is_hard(components.size(), false);
        vector<pair<long long,vector<int>>> tree_results(components.size());

        for (int ci = 0; ci < (int)components.size(); ci++) {
            auto& comp = components[ci];
            long long sc = 0;
            vector<int> res;
            if (solve_as_tree(comp, res, sc)) {
                tree_results[ci] = {sc, res};
                is_hard[ci] = false;
            } else {
                is_hard[ci] = true;
                total_hard_nodes += comp.size();
            }
        }

        for (int ci = 0; ci < (int)components.size(); ci++) {
            if (!is_hard[ci]) {
                final_weight_global += tree_results[ci].first;
                for (int v : tree_results[ci].second) final_sol_global.push_back(v);
            }
        }

        double time_remaining = TIME_LIMIT - elapsed_sec();

        for (int ci = 0; ci < (int)components.size() && time_ok(); ci++) {
            if (!is_hard[ci]) continue;
            auto& comp = components[ci];

            double share = (total_hard_nodes > 0)
                           ? time_remaining * (double)comp.size() / total_hard_nodes
                           : time_remaining;
            share = max(1.0, share);

            ils_component(comp, share, final_weight_global, final_sol_global);
        }
    }

output:
    unfold_solution();
    print_solution();
    return 0;
}
