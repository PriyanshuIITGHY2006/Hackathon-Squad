// ─────────────────────────────────────────────────────────────────────────────
//  mwis_engine — Maximum Weight Independent Set solver with a live trace stream
//
//  Pipeline:   kernelize  →  LP/Nemhauser-Trotter  →  per-component solve
//              (exact tree DP | exact branch & bound | ILS)  →  unfold  →  verify
//
//  Every reduction rule below carries the proof obligation it satisfies.  Two
//  rules differ deliberately from solution.cpp because the versions there are
//  unsound; see RULE R3 and RULE R4 for the counterexamples.
//
//  Input  (stdin or --input FILE):
//      N M
//      w_1 w_2 ... w_N
//      u v            (M lines, 1-indexed, undirected)
//
//  Output:
//      --trace   newline-delimited JSON events on stdout (streamed, flushed)
//      default   judge format: total weight, then the sorted vertex list
//
//  No wall-clock limit is imposed unless --time-limit is given.  SIGTERM and
//  SIGINT stop the search cooperatively and still emit a complete, verified
//  answer, so an operator can stop a run at any moment without losing it.
// ─────────────────────────────────────────────────────────────────────────────
#include <bits/stdc++.h>
#include <csignal>
using namespace std;
using ll = long long;

// ── run control ──────────────────────────────────────────────────────────────
static volatile sig_atomic_t g_stop = 0;
static void on_signal(int) { g_stop = 1; }

static chrono::steady_clock::time_point START_TIME;
static inline double elapsed_sec() {
    return chrono::duration<double>(chrono::steady_clock::now() - START_TIME).count();
}
static inline ll elapsed_ms() { return (ll)llround(elapsed_sec() * 1000.0); }

struct Options {
    bool   trace           = false;
    string input           = "";
    double time_limit      = 0.0;      // 0 = unlimited
    ll     stall           = 2000;     // non-improving ILS iterations per component; 0 = unlimited
    ll     max_iters       = 0;        // 0 = unlimited
    unsigned seed          = 12345u;
    int    exact_threshold = 100;      // component size eligible for exact branch & bound
    ll     exact_nodes     = 3000000;  // B&B node budget per component
    bool   no_reduce       = false;    // ablation: skip kernelization entirely
    bool   legacy_rules    = false;    // reproduce solution.cpp's unsound R3/R4 (for the bug demo)
    set<string> disabled;              // rule ids switched off for ablation
    int    max_search_events = 4000;   // cap on per-move local-search events
};
static Options OPT;

static inline bool rule_on(const char* id) { return !OPT.disabled.count(id); }
static inline bool time_ok() {
    if (g_stop) return false;
    if (OPT.time_limit <= 0.0) return true;
    return elapsed_sec() < OPT.time_limit;
}

// ── tiny JSON writer (no dependencies, one object per line) ───────────────────
static string jesc(const string& s) {
    string o; o.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
            case '"':  o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n";  break;
            case '\r': o += "\\r";  break;
            case '\t': o += "\\t";  break;
            default:
                if ((unsigned char)c < 0x20) { char b[8]; snprintf(b, sizeof b, "\\u%04x", c); o += b; }
                else o += c;
        }
    }
    return o;
}
struct J {
    string s = "{";
    bool first = true;
    J& key(const char* k) { if (!first) s += ','; first = false; s += '"'; s += k; s += "\":"; return *this; }
    J& str(const char* k, const string& v) { key(k); s += '"'; s += jesc(v); s += '"'; return *this; }
    J& num(const char* k, ll v)            { key(k); s += to_string(v); return *this; }
    J& dbl(const char* k, double v)        { key(k); char b[40]; snprintf(b, sizeof b, "%.6g", v); s += b; return *this; }
    J& boo(const char* k, bool v)          { key(k); s += (v ? "true" : "false"); return *this; }
    J& arr(const char* k, const vector<int>& v) {
        key(k); s += '[';
        for (size_t i = 0; i < v.size(); i++) { if (i) s += ','; s += to_string(v[i]); }
        s += ']'; return *this;
    }
    J& arrll(const char* k, const vector<ll>& v) {
        key(k); s += '[';
        for (size_t i = 0; i < v.size(); i++) { if (i) s += ','; s += to_string(v[i]); }
        s += ']'; return *this;
    }
    J& raw(const char* k, const string& v) { key(k); s += v; return *this; }
    string done() { return s + "}"; }
};

static ll g_event_seq = 0;
static void emit(J& j) {
    if (!OPT.trace) return;
    j.num("seq", g_event_seq++).num("ms", elapsed_ms());
    fputs(j.done().c_str(), stdout);
    fputc('\n', stdout);
    fflush(stdout);
}
static void emit_msg(const char* ev, const string& msg) {
    if (!OPT.trace) return;
    J j; j.str("e", ev).str("msg", msg); emit(j);
}

// ── graph state ──────────────────────────────────────────────────────────────
static int N = 0, M = 0;
static vector<ll>          W0;     // original weights (never modified)
static vector<ll>          W;      // working weights (folds rewrite these)
static vector<vector<int>> adj;    // sorted adjacency, mutated by folds
static vector<char>        removed, forced_in;
static vector<int>         live_deg;

static inline bool has_edge(int u, int v) {
    return binary_search(adj[u].begin(), adj[u].end(), v);
}
static inline void add_edge_dyn(int u, int v) {
    auto it = lower_bound(adj[u].begin(), adj[u].end(), v);
    if (it == adj[u].end() || *it != v) adj[u].insert(it, v);
}
static inline void mark_removed(int v) {
    removed[v] = true;
    for (int u : adj[v]) if (!removed[u]) live_deg[u]--;
}
static vector<int> include_vertex(int v) {
    // Fixing v IN forces every currently active neighbour OUT — the independent
    // set constraint, applied to the whole live neighbourhood and not merely to
    // the vertices the calling rule happened to name.  The evicted vertices are
    // returned so the trace can report exactly what each rule decided.
    forced_in[v] = true;
    vector<int> nbrs;
    for (int u : adj[v]) if (!removed[u]) nbrs.push_back(u);
    mark_removed(v);
    vector<int> evicted;
    for (int u : nbrs) if (!removed[u]) { mark_removed(u); evicted.push_back(u); }
    return evicted;
}
static vector<int> active_neighbors(int v) {
    vector<int> r;
    for (int u : adj[v]) if (!removed[u]) r.push_back(u);
    return r;
}

// ── fold bookkeeping ─────────────────────────────────────────────────────────
//  type 1 (N-fold, degree-1): leaf v folded into neighbour a.
//        reconstruct: a ∉ IS  ⇒  v ∈ IS
//  type 2 (V-fold, degree-2): v and b folded into supernode a.
//        reconstruct: a ∈ IS  ⇒  b ∈ IS ;  a ∉ IS  ⇒  v ∈ IS
struct Fold { int type, v, a, b; };
static vector<Fold> folds;
static ll fold_offset = 0;

// ── statistics ───────────────────────────────────────────────────────────────
static map<string, ll> rule_counts;
static void count_rule(const char* id) { rule_counts[id]++; }

// ─────────────────────────────────────────────────────────────────────────────
//  REDUCTION RULES
//
//  Every rule must preserve at least one optimal solution.  The proof for each
//  is stated where it is implemented; anything unproven is not applied.
// ─────────────────────────────────────────────────────────────────────────────

// RULE R0 — isolated vertex.
//   deg(v) = 0 ⇒ v conflicts with nothing ⇒ v is in some optimal solution.
static bool rule_isolated(int v) {
    if (live_deg[v] != 0) return false;
    if (!rule_on("R0")) return false;
    vector<int> evicted = include_vertex(v);
    count_rule("R0");
    if (OPT.trace) {
        J j; j.str("e", "rule").str("rule", "R0").str("name", "isolated vertex")
             .num("v", v).num("w", W[v]).str("decide", "in").arr("out", evicted)
             .str("msg", "deg(v" + to_string(v) + ") = 0, weight " + to_string(W[v]) +
                         " — nothing conflicts with it, so it is in some optimal solution.");
        emit(j);
    }
    return true;
}

// RULE R1 — degree-1 (include or N-fold).
//   Let v be a leaf with neighbour u.
//   (a) w(v) ≥ w(u): any solution using u can swap u→v without losing weight
//       (v's only neighbour is u), so v is in some optimal solution.
//   (b) w(v) < w(u): fold.  In the reduced graph G−v with w'(u) = w(u) − w(v)
//       and offset += w(v):  u ∈ IS' ⇒ real weight w'(u)+offset = w(u);
//                            u ∉ IS' ⇒ v joins, real weight = offset = w(v).
//       Both cases match OPT exactly, so OPT(G) = w(v) + OPT(G−v, w').
static bool rule_degree_one(int v) {
    if (live_deg[v] != 1) return false;
    if (!rule_on("R1")) return false;
    int u = -1;
    for (int x : adj[v]) if (!removed[x]) { u = x; break; }
    if (u < 0) return false;

    if (W[v] >= W[u]) {
        ll wv = W[v], wu = W[u];
        vector<int> evicted = include_vertex(v);
        count_rule("R1-include");
        if (OPT.trace) {
            J j; j.str("e", "rule").str("rule", "R1").str("name", "degree-1 include")
                 .num("v", v).num("u", u).num("w", wv).str("decide", "in").arr("out", evicted)
                 .str("msg", "v" + to_string(v) + " is a leaf of v" + to_string(u) + "; w=" +
                             to_string(wv) + " ≥ " + to_string(wu) +
                             " — swapping u→v never loses weight, so include v" + to_string(v) + ".");
            emit(j);
        }
    } else {
        ll wv = W[v], wu = W[u];
        folds.push_back({1, v, u, -1});
        fold_offset += W[v];
        W[u] -= W[v];
        mark_removed(v);
        count_rule("R1-fold");
        if (OPT.trace) {
            J j; j.str("e", "rule").str("rule", "R1F").str("name", "degree-1 N-fold")
                 .num("v", v).num("u", u).num("neww", W[u]).num("offset", fold_offset)
                 .num("fold_id", (ll)folds.size() - 1).str("decide", "fold")
                 .str("msg", "leaf v" + to_string(v) + " (w=" + to_string(wv) + ") < v" + to_string(u) +
                             " (w=" + to_string(wu) + "): fold it away, w(v" + to_string(u) + ") ← " +
                             to_string(wu) + "−" + to_string(wv) + " = " + to_string(W[u]) +
                             ", offset += " + to_string(wv) + ".");
            emit(j);
        }
    }
    return true;
}

// RULE R2 — neighbourhood removal.
//   w(v) ≥ Σ_{u∈N(v)} w(u)  ⇒  v is in some optimal solution.
//   Proof: take optimal S with v ∉ S and X = S ∩ N(v).  S∖X ∪ {v} is
//   independent and w(v) ≥ Σ_{N(v)} ≥ w(X), so it is at least as heavy.
static bool rule_neighborhood_removal(int v) {
    if (!rule_on("R2")) return false;
    int d = live_deg[v];
    if (d <= 0 || d > 64) return false;
    ll sum = 0;
    for (int u : adj[v]) if (!removed[u]) sum += W[u];
    if (W[v] < sum) return false;
    ll wv = W[v];
    vector<int> evicted = include_vertex(v);
    count_rule("R2");
    if (OPT.trace) {
        J j; j.str("e", "rule").str("rule", "R2").str("name", "neighbourhood removal")
             .num("v", v).num("w", wv).num("nbsum", sum).str("decide", "in").arr("out", evicted)
             .str("msg", "w(v" + to_string(v) + ") = " + to_string(wv) + " ≥ Σ w(N(v)) = " +
                         to_string(sum) + " — taking v beats taking any subset of its neighbours.");
        emit(j);
    }
    return true;
}

// RULE R3 — simplicial vertex (the sound form of the "triangle" rule).
//   If N(v) induces a clique and w(v) ≥ max_{u∈N(v)} w(u), then v is in some
//   optimal solution.  Proof: a clique meets any independent set in ≤ 1 vertex,
//   so |S ∩ N(v)| ≤ 1.  Empty ⇒ add v.  {u} ⇒ swap u→v, weight does not drop.
//
//   solution.cpp instead includes the heaviest of {v,a,b} for a degree-2
//   triangle.  That is unsound when the heaviest is a neighbour a, because a
//   may have edges outside the triangle that an optimal solution needs:
//       v–a, v–b, a–b, plus a–x with w(x) ≫ w(a).
//   Including a forfeits x.  Here the heaviest-neighbour case falls through to
//   R4 (v is then dominated by that neighbour), which is sound.
static bool rule_simplicial(int v) {
    if (!rule_on("R3")) return false;
    int d = live_deg[v];
    if (d <= 0 || d > 12) return false;
    vector<int> nb = active_neighbors(v);

    if (OPT.legacy_rules) {
        // Reproduction of solution.cpp's unsound degree-2 triangle rule, kept
        // behind a flag so the correctness harness can measure the difference.
        if (d != 2) return false;
        int a = nb[0], b = nb[1];
        if (!has_edge(a, b)) return false;
        ll best = max({W[v], W[a], W[b]});
        int win = (W[v] == best) ? v : (W[a] == best ? a : b);
        include_vertex(win);
        count_rule("R3-legacy");
        return true;
    }

    ll mx = 0;
    for (int u : nb) mx = max(mx, W[u]);
    if (W[v] < mx) return false;
    for (size_t i = 0; i < nb.size(); i++)
        for (size_t k = i + 1; k < nb.size(); k++)
            if (!has_edge(nb[i], nb[k])) return false;   // N(v) is not a clique

    ll wv = W[v];
    vector<int> evicted = include_vertex(v);
    count_rule("R3");
    if (OPT.trace) {
        J j; j.str("e", "rule").str("rule", "R3").str("name", "simplicial vertex")
             .num("v", v).num("w", wv).num("deg", d).arr("nb", nb).str("decide", "in")
             .arr("out", evicted)
             .str("msg", "N(v" + to_string(v) + ") is a clique of size " + to_string(d) +
                         " and w=" + to_string(wv) + " ≥ heaviest neighbour " + to_string(mx) +
                         " — at most one of the clique can be chosen, so choose v" + to_string(v) + ".");
        emit(j);
    }
    return true;
}

// RULE R4 — domination.
//   If u ∈ N(v) with N[u] ⊆ N[v] and w(u) ≥ w(v), then v may be discarded.
//   Proof: take optimal S with v ∈ S.  u ∉ S (u is adjacent to v).  Every
//   x ∈ N(u) lies in N[v]; x ≠ v ⇒ x ∈ N(v) ⇒ x ∉ S.  So S∖{v} ∪ {u} is
//   independent and weighs w(S) − w(v) + w(u) ≥ w(S).
//
//   The containment direction matters.  solution.cpp tests N[v] ⊆ N[u], which
//   is the reverse and is unsound: with v–u, u–x, w(v)=w(u)=10, w(x)=100 we get
//   N[v] ⊆ N[u] and w(u) ≥ w(v), yet discarding v loses the optimum {v,x}.
static bool rule_domination(int v) {
    if (!rule_on("R4")) return false;
    int d = live_deg[v];
    if (d <= 0 || d > 16) return false;

    for (int u : adj[v]) {
        if (removed[u] || W[u] < W[v]) continue;
        if (OPT.legacy_rules) {
            // solution.cpp's direction: N[v] ⊆ N[u] (unsound, flag-gated).
            bool dom = true;
            for (int x : adj[v]) {
                if (removed[x] || x == u) continue;
                if (!has_edge(u, x)) { dom = false; break; }
            }
            if (!dom) continue;
        } else {
            if (live_deg[u] > 64) continue;
            bool dom = true;                       // N[u] ⊆ N[v] ?
            for (int x : adj[u]) {
                if (removed[x] || x == v) continue;
                if (!has_edge(v, x)) { dom = false; break; }
            }
            if (!dom) continue;
        }
        ll wv = W[v], wu = W[u];
        mark_removed(v);
        count_rule(OPT.legacy_rules ? "R4-legacy" : "R4");
        if (OPT.trace) {
            J j; j.str("e", "rule").str("rule", "R4").str("name", "domination")
                 .num("v", v).num("u", u).num("w", wv).str("decide", "out")
                 .str("msg", "N[v" + to_string(u) + "] ⊆ N[v" + to_string(v) + "] and w(v" +
                             to_string(u) + ")=" + to_string(wu) + " ≥ w(v" + to_string(v) + ")=" +
                             to_string(wv) + " — v" + to_string(u) + " dominates v" + to_string(v) +
                             ", discard v" + to_string(v) + ".");
            emit(j);
        }
        return true;
    }
    return false;
}

// RULE R5 — degree-2 V-fold.
//   deg(v)=2, N(v)={a,b} with a ≁ b and max(w(a),w(b)) ≤ w(v) < w(a)+w(b).
//   Any optimal solution restricted to {v,a,b} is {v}, {a,b}, {a} or {b}; the
//   single-neighbour cases are dominated by {v} because w(v) ≥ max(w(a),w(b))
//   and v is free whenever exactly one of a,b is chosen.  So the choice is
//   {v} versus {a,b}, which is exactly what the supernode encodes:
//       w(a') = w(a)+w(b)−w(v),  N(a') = (N(a) ∪ N(b)) ∖ {v},  offset += w(v).
//   a' ∈ IS ⇒ real {a,b} (w(a')+offset = w(a)+w(b));  a' ∉ IS ⇒ real {v}.
//
//   The max(w(a),w(b)) ≤ w(v) guard is essential.  Without it (solution.cpp
//   folds on w(v) < w(a)+w(b) alone) the "take a alone" case is lost: with
//   w(a)=100, w(b)=1, w(v)=2 and a heavy vertex hanging off b, folding forfeits
//   a.  With the guard the fold is sound at any stage, so it needs no LP
//   precondition.
static bool rule_degree_two_fold(int v) {
    if (!rule_on("R5")) return false;
    if (live_deg[v] != 2) return false;
    vector<int> nb = active_neighbors(v);
    if (nb.size() != 2) return false;
    int a = nb[0], b = nb[1];
    if (has_edge(a, b)) return false;
    if (W[v] >= W[a] + W[b]) return false;                       // handled by R2
    if (!OPT.legacy_rules && W[v] < max(W[a], W[b])) return false; // unsound without this

    ll wv = W[v], wa = W[a], wb = W[b];
    folds.push_back({2, v, a, b});
    fold_offset += W[v];
    W[a] = W[a] + W[b] - W[v];

    // N(a) ← (N(a) ∪ N(b)) ∖ {v}
    vector<int> moved;
    for (int x : adj[b]) {
        if (removed[x] || x == v || x == a) continue;
        if (!has_edge(a, x)) { add_edge_dyn(a, x); add_edge_dyn(x, a); moved.push_back(x); }
    }
    mark_removed(v);
    mark_removed(b);
    live_deg[a] = 0;
    for (int x : adj[a]) if (!removed[x]) live_deg[a]++;
    for (int x : moved) { live_deg[x] = 0; for (int y : adj[x]) if (!removed[y]) live_deg[x]++; }

    count_rule("R5");
    if (OPT.trace) {
        J j; j.str("e", "rule").str("rule", "R5").str("name", "degree-2 V-fold")
             .num("v", v).num("a", a).num("b", b).num("neww", W[a]).num("offset", fold_offset)
             .num("fold_id", (ll)folds.size() - 1).arr("inherited", moved).str("decide", "fold")
             .str("msg", "v" + to_string(v) + " has non-adjacent neighbours v" + to_string(a) +
                         ", v" + to_string(b) + " with max(" + to_string(wa) + "," + to_string(wb) +
                         ") ≤ " + to_string(wv) + " < " + to_string(wa + wb) +
                         " — merge v" + to_string(b) + " into v" + to_string(a) + ", w ← " +
                         to_string(wa) + "+" + to_string(wb) + "−" + to_string(wv) + " = " +
                         to_string(W[a]) + ".");
        emit(j);
    }
    return true;
}

// RULE R6 — weighted twins.
//   u ≁ v with N(u) = N(v) and w(u)+w(v) ≥ Σ_{x∈N(v)} w(x) ⇒ both are in some
//   optimal solution.  Proof: for optimal S let X = S ∩ N(v).  If X = ∅ we may
//   add both.  Otherwise u,v ∉ S and S∖X ∪ {u,v} is independent with weight
//   w(S) − w(X) + w(u) + w(v) ≥ w(S).
static int twin_pass(deque<int>& Q, vector<char>& inQ) {
    if (!rule_on("R6")) return 0;
    unordered_map<unsigned long long, vector<int>> buckets;
    for (int v = 1; v <= N; v++) {
        if (removed[v] || live_deg[v] == 0 || live_deg[v] > 16) continue;
        unsigned long long h = 1469598103934665603ULL;
        for (int u : adj[v]) if (!removed[u]) { h ^= (unsigned long long)u; h *= 1099511628211ULL; }
        buckets[h ^ (unsigned long long)live_deg[v]].push_back(v);
    }
    int applied = 0;
    for (auto& [h, group] : buckets) {
        if (group.size() < 2) continue;
        for (size_t i = 0; i < group.size(); i++) {
            int u = group[i];
            if (removed[u]) continue;
            for (size_t k = i + 1; k < group.size(); k++) {
                int v = group[k];
                if (removed[v] || removed[u]) continue;
                if (has_edge(u, v)) continue;
                vector<int> nu = active_neighbors(u), nv = active_neighbors(v);
                if (nu != nv || nu.empty()) continue;
                ll sum = 0;
                for (int x : nu) sum += W[x];
                if (W[u] + W[v] < sum) continue;
                ll wu = W[u], wv = W[v];
                for (int x : nu) if (!inQ[x]) { Q.push_back(x); inQ[x] = true; }
                vector<int> evicted = include_vertex(u);
                if (!removed[v]) { auto e2 = include_vertex(v); evicted.insert(evicted.end(), e2.begin(), e2.end()); }
                else forced_in[v] = true;
                count_rule("R6");
                applied++;
                if (OPT.trace) {
                    J j; j.str("e", "rule").str("rule", "R6").str("name", "weighted twins")
                         .num("v", v).num("u", u).num("nbsum", sum).arr("nb", nu).str("decide", "in")
                         .arr("out", evicted)
                         .str("msg", "v" + to_string(u) + " and v" + to_string(v) +
                                     " are non-adjacent twins with w " + to_string(wu) + "+" +
                                     to_string(wv) + " ≥ Σ w(N) = " + to_string(sum) +
                                     " — include both.");
                    emit(j);
                }
                break;
            }
        }
    }
    return applied;
}

// ── Dinic max-flow, used by the Nemhauser-Trotter LP reduction ────────────────
struct Dinic {
    struct Edge { int to, rev; ll cap; };
    int n;
    vector<vector<Edge>> g;
    vector<int> level, cur;
    explicit Dinic(int n) : n(n), g(n), level(n), cur(n) {}
    void add_edge(int u, int v, ll cap) {
        g[u].push_back({v, (int)g[v].size(), cap});
        g[v].push_back({u, (int)g[u].size() - 1, 0LL});
    }
    bool bfs(int s, int t) {
        fill(level.begin(), level.end(), -1);
        queue<int> q; level[s] = 0; q.push(s);
        while (!q.empty()) {
            int v = q.front(); q.pop();
            for (auto& e : g[v]) if (e.cap > 0 && level[e.to] < 0) { level[e.to] = level[v] + 1; q.push(e.to); }
        }
        return level[t] >= 0;
    }
    ll dfs(int v, int t, ll f) {
        if (v == t) return f;
        for (int& i = cur[v]; i < (int)g[v].size(); i++) {
            Edge& e = g[v][i];
            if (e.cap <= 0 || level[e.to] != level[v] + 1) continue;
            ll d = dfs(e.to, t, min(f, e.cap));
            if (d > 0) { e.cap -= d; g[e.to][e.rev].cap += d; return d; }
        }
        return 0;
    }
    ll max_flow(int s, int t) {
        ll flow = 0;
        while (bfs(s, t)) {
            fill(cur.begin(), cur.end(), 0);
            ll d;
            while ((d = dfs(s, t, (ll)4e18)) > 0) flow += d;
        }
        return flow;
    }
    vector<char> reachable(int s) {
        vector<char> vis(n, 0); queue<int> q; vis[s] = 1; q.push(s);
        while (!q.empty()) {
            int v = q.front(); q.pop();
            for (auto& e : g[v]) if (e.cap > 0 && !vis[e.to]) { vis[e.to] = 1; q.push(e.to); }
        }
        return vis;
    }
};

// RULE R7 — Nemhauser-Trotter LP reduction.
//   The LP relaxation of MWIS is half-integral.  Solving it as a min-cut on the
//   bipartite double cover (s→v_L and v_R→t with capacity w(v); v_L→u_R and
//   u_L→v_R with ∞ for each edge) gives, from residual reachability of s:
//       v_L reachable, v_R not  ⇒ x*(v) = 1 ⇒ v is in some optimal solution
//       v_L not,       v_R yes  ⇒ x*(v) = 0 ⇒ v is in no optimal solution
//       otherwise               ⇒ x*(v) = ½ ⇒ v stays in the hard kernel
static int lp_reduce(deque<int>& Q, vector<char>& inQ) {
    if (!rule_on("R7")) return 0;
    const ll INF = (ll)4e18;
    const int S = 0, T = 2 * N + 1;
    int live = 0;
    for (int v = 1; v <= N; v++) if (!removed[v]) live++;
    if (live == 0) return 0;

    Dinic din(2 * N + 2);
    for (int v = 1; v <= N; v++) {
        if (removed[v]) continue;
        din.add_edge(S, v, W[v]);
        din.add_edge(N + v, T, W[v]);
    }
    for (int v = 1; v <= N; v++) {
        if (removed[v]) continue;
        for (int u : adj[v]) {
            if (removed[u] || u <= v) continue;
            din.add_edge(v, N + u, INF);
            din.add_edge(u, N + v, INF);
        }
    }
    ll flow = din.max_flow(S, T);
    auto R = din.reachable(S);

    vector<int> inc, exc, half;
    for (int v = 1; v <= N; v++) {
        if (removed[v]) continue;
        bool lv = R[v], rv = R[N + v];
        if (lv && !rv)      inc.push_back(v);
        else if (!lv && rv) exc.push_back(v);
        else                half.push_back(v);
    }
    if (OPT.trace) {
        J j; j.str("e", "lp").num("flow", flow).num("live", live)
             .arr("forced_in", inc).arr("forced_out", exc).arr("half", half)
             .str("msg", "Nemhauser-Trotter on " + to_string(live) + " live vertices: max-flow " +
                         to_string(flow) + " ⇒ " + to_string(inc.size()) + " vertices at x*=1, " +
                         to_string(exc.size()) + " at x*=0, " + to_string(half.size()) +
                         " at x*=½ (the hard kernel).");
        emit(j);
    }
    auto enq = [&](int u) { if (!removed[u] && !inQ[u]) { Q.push_back(u); inQ[u] = true; } };
    int decided = 0;
    vector<int> evicted;
    for (int v : exc) if (!removed[v]) { for (int u : adj[v]) enq(u); mark_removed(v); decided++; count_rule("R7-out"); }
    for (int v : inc) if (!removed[v]) {
        for (int u : adj[v]) enq(u);
        auto e2 = include_vertex(v);
        evicted.insert(evicted.end(), e2.begin(), e2.end());
        decided++; count_rule("R7-in");
    }
    if (OPT.trace && !evicted.empty()) {
        J j; j.str("e", "lp_evict").arr("out", evicted)
             .str("msg", to_string(evicted.size()) + " neighbour(s) of the x*=1 vertices are forced out.");
        emit(j);
    }
    return decided;
}

// ── kernelization driver ─────────────────────────────────────────────────────
static vector<int> kernelize() {
    deque<int> Q;
    vector<char> inQ(N + 1, 0);
    for (int v = 1; v <= N; v++) { Q.push_back(v); inQ[v] = 1; }

    auto enqueue_neighbors = [&](int v) {
        for (int u : adj[v]) if (!removed[u] && !inQ[u]) { Q.push_back(u); inQ[u] = 1; }
    };

    auto drain = [&](const char* phase) {
        int before_live = 0;
        for (int v = 1; v <= N; v++) if (!removed[v]) before_live++;
        while (!Q.empty() && time_ok()) {
            int v = Q.front(); Q.pop_front();
            inQ[v] = 0;
            if (removed[v]) continue;
            bool applied =
                rule_isolated(v)             ||
                rule_degree_one(v)           ||
                rule_neighborhood_removal(v) ||
                rule_simplicial(v)           ||
                rule_domination(v)           ||
                rule_degree_two_fold(v);
            if (applied) enqueue_neighbors(v);
        }
        int after_live = 0;
        for (int v = 1; v <= N; v++) if (!removed[v]) after_live++;
        if (OPT.trace) {
            J j; j.str("e", "phase").str("phase", phase).num("live", after_live)
                 .num("removed", before_live - after_live)
                 .str("msg", string(phase) + ": " + to_string(before_live - after_live) +
                             " vertices decided, " + to_string(after_live) + " still live.");
            emit(j);
        }
    };

    emit_msg("phase_start", "Phase 1 — local reduction rules (R0 isolated, R1 degree-1, "
                            "R2 neighbourhood removal, R3 simplicial, R4 domination, R5 V-fold).");
    drain("reduce-1");

    if (time_ok() && twin_pass(Q, inQ) > 0) {
        emit_msg("phase_start", "Phase 1b — twin reduction fired, re-running local rules.");
        drain("reduce-1b");
    }

    if (time_ok()) {
        emit_msg("phase_start", "Phase 2 — Nemhauser-Trotter LP reduction (Dinic max-flow).");
        int decided = lp_reduce(Q, inQ);
        if (decided > 0) {
            emit_msg("phase_start", "Phase 3 — local rules again on the LP-reduced graph.");
            drain("reduce-2");
        }
    }

    vector<int> kernel;
    for (int v = 1; v <= N; v++) if (!removed[v]) kernel.push_back(v);
    return kernel;
}

// ── components ───────────────────────────────────────────────────────────────
static vector<vector<int>> get_components(const vector<int>& kernel) {
    vector<char> inK(N + 1, 0), vis(N + 1, 0);
    for (int v : kernel) inK[v] = 1;
    vector<vector<int>> comps;
    for (int root : kernel) {
        if (vis[root]) continue;
        vector<int> comp; queue<int> q;
        q.push(root); vis[root] = 1;
        while (!q.empty()) {
            int v = q.front(); q.pop();
            comp.push_back(v);
            for (int u : adj[v]) if (!removed[u] && inK[u] && !vis[u]) { vis[u] = 1; q.push(u); }
        }
        sort(comp.begin(), comp.end());
        comps.push_back(move(comp));
    }
    return comps;
}
static ll component_edges(const vector<int>& comp) {
    unordered_set<int> s(comp.begin(), comp.end());
    ll e = 0;
    for (int v : comp) for (int u : adj[v]) if (!removed[u] && s.count(u)) e++;
    return e / 2;
}

// ── exact tree DP (forest components solve in O(n)) ──────────────────────────
static bool solve_tree(const vector<int>& comp, vector<int>& out, ll& score) {
    if (component_edges(comp) != (ll)comp.size() - 1) return false;
    unordered_map<int, ll> dp_in, dp_out;
    unordered_map<int, int> par;
    vector<int> order;
    order.reserve(comp.size());
    unordered_set<int> inC(comp.begin(), comp.end());
    queue<int> q;
    int root = comp[0];
    q.push(root); par[root] = -1;
    unordered_set<int> vis{root};
    while (!q.empty()) {
        int v = q.front(); q.pop();
        order.push_back(v);
        for (int u : adj[v]) if (!removed[u] && inC.count(u) && !vis.count(u)) { vis.insert(u); par[u] = v; q.push(u); }
    }
    for (int i = (int)order.size() - 1; i >= 0; i--) {
        int v = order[i];
        ll din = W[v], dout = 0;
        for (int u : adj[v]) {
            if (removed[u] || !inC.count(u)) continue;
            if (par.count(u) && par[u] == v) { din += dp_out[u]; dout += max(dp_in[u], dp_out[u]); }
        }
        dp_in[v] = din; dp_out[v] = dout;
    }
    unordered_map<int, bool> take;
    take[root] = dp_in[root] >= dp_out[root];
    for (int v : order) {
        for (int u : adj[v]) {
            if (removed[u] || !inC.count(u)) continue;
            if (!(par.count(u) && par[u] == v)) continue;
            take[u] = take[v] ? false : (dp_in[u] >= dp_out[u]);
        }
    }
    score = 0; out.clear();
    for (int v : comp) if (take[v]) { out.push_back(v); score += W[v]; }
    return true;
}

// ── exact branch & bound for small components ────────────────────────────────
//  Bitset representation; upper bound by greedy clique cover (an independent
//  set takes at most one vertex from each clique, so the bound is the sum of
//  per-clique maxima).  Returns true when optimality was proven within budget.
static const int EXB = 256;
struct ExactBB {
    int n = 0;
    vector<bitset<EXB>> nb;
    vector<ll> w;
    ll best = 0;
    bitset<EXB> best_sel;
    ll nodes = 0, budget = 0;
    bool proven = true;

    ll bound(const bitset<EXB>& mask) const {
        ll b = 0;
        bitset<EXB> rem = mask;
        while (rem.any()) {
            int v = -1;
            for (int i = 0; i < n; i++) if (rem[i]) { v = i; break; }
            if (v < 0) break;
            bitset<EXB> clique; clique.set(v);
            bitset<EXB> cand = nb[v] & rem;
            ll mx = w[v];
            while (cand.any()) {
                int u = -1;
                for (int i = 0; i < n; i++) if (cand[i]) { u = i; break; }
                if (u < 0) break;
                clique.set(u);
                mx = max(mx, w[u]);
                cand &= nb[u];
            }
            b += mx;
            rem &= ~clique;
        }
        return b;
    }
    void rec(bitset<EXB> mask, ll cur, bitset<EXB> sel) {
        if (g_stop || !time_ok()) { proven = false; return; }
        if (++nodes > budget) { proven = false; return; }

        // free vertices (no live neighbour) are always worth taking
        bool changed = true;
        while (changed) {
            changed = false;
            for (int i = 0; i < n && !changed; i++) {
                if (!mask[i]) continue;
                if ((nb[i] & mask).none()) { mask.reset(i); sel.set(i); cur += w[i]; changed = true; }
            }
        }
        if (cur > best) { best = cur; best_sel = sel; }
        if (mask.none()) return;
        if (cur + bound(mask) <= best) return;

        int pick = -1, pd = -1;
        for (int i = 0; i < n; i++) {
            if (!mask[i]) continue;
            int d = (int)(nb[i] & mask).count();
            if (d > pd) { pd = d; pick = i; }
        }
        if (pick < 0) return;

        bitset<EXB> m1 = mask;                 // branch: take pick
        m1.reset(pick);
        m1 &= ~nb[pick];
        bitset<EXB> s1 = sel; s1.set(pick);
        rec(m1, cur + w[pick], s1);

        bitset<EXB> m0 = mask;                 // branch: drop pick
        m0.reset(pick);
        rec(m0, cur, sel);
    }
};
static bool solve_exact(const vector<int>& comp, vector<int>& out, ll& score, ll& nodes_used) {
    int n = (int)comp.size();
    if (n > EXB || n > OPT.exact_threshold) return false;
    ExactBB bb;
    bb.n = n;
    bb.nb.assign(n, bitset<EXB>());
    bb.w.assign(n, 0);
    unordered_map<int, int> idx;
    for (int i = 0; i < n; i++) { idx[comp[i]] = i; bb.w[i] = W[comp[i]]; }
    for (int i = 0; i < n; i++)
        for (int u : adj[comp[i]]) {
            if (removed[u]) continue;
            auto it = idx.find(u);
            if (it != idx.end()) bb.nb[i].set(it->second);
        }
    bb.budget = OPT.exact_nodes;
    bitset<EXB> full;
    for (int i = 0; i < n; i++) full.set(i);
    bb.rec(full, 0, bitset<EXB>());
    nodes_used = bb.nodes;
    if (!bb.proven) return false;
    score = bb.best;
    out.clear();
    for (int i = 0; i < n; i++) if (bb.best_sel[i]) out.push_back(comp[i]);
    return true;
}

// ── local search + ILS (for components too large to certify) ──────────────────
static vector<char> in_sol;
static vector<int>  conf;
static ll           cur_weight = 0;
static int          search_events = 0;

static inline void add_to_sol(int v) {
    in_sol[v] = 1; cur_weight += W[v];
    for (int u : adj[v]) if (!removed[u]) conf[u]++;
}
static inline void remove_from_sol(int v) {
    in_sol[v] = 0; cur_weight -= W[v];
    for (int u : adj[v]) if (!removed[u]) conf[u]--;
}
static void reset_sol(const vector<int>& comp) {
    for (int v : comp) { in_sol[v] = 0; conf[v] = 0; }
    cur_weight = 0;
}
static void emit_move(int comp_id, const char* move, const vector<int>& added,
                      const vector<int>& dropped, ll weight) {
    if (!OPT.trace) return;
    if (search_events++ > OPT.max_search_events) return;
    J j; j.str("e", "move").num("comp", comp_id).str("move", move)
         .arr("add", added).arr("drop", dropped).num("weight", weight);
    emit(j);
}
static void greedy_build(const vector<int>& comp, mt19937& rng, double randomness) {
    reset_sol(comp);
    vector<pair<double, int>> scored;
    scored.reserve(comp.size());
    uniform_real_distribution<double> noise(1.0 - randomness, 1.0 + randomness);
    for (int v : comp) {
        double score = (double)W[v] / (live_deg[v] + 1);
        if (randomness > 0.0) score *= noise(rng);
        scored.push_back({score, v});
    }
    sort(scored.rbegin(), scored.rend());
    for (auto& [sc, v] : scored) if (!in_sol[v] && conf[v] == 0) add_to_sol(v);
}
static void make_maximal(const vector<int>& comp) {
    for (int v : comp) if (!in_sol[v] && conf[v] == 0) add_to_sol(v);
}
// PROBE: a (1→k) swap — one entering vertex replacing all of its IS neighbours.
static bool probe_pass(const vector<int>& comp, int comp_id) {
    bool improved = false;
    for (int u : comp) {
        if (in_sol[u]) continue;
        if (conf[u] == 0) { add_to_sol(u); emit_move(comp_id, "probe-free", {u}, {}, cur_weight); improved = true; continue; }
        ll gain = W[u];
        vector<int> nbrs;
        for (int v : adj[u]) if (!removed[v] && in_sol[v]) { gain -= W[v]; nbrs.push_back(v); }
        if (gain > 0) {
            for (int v : nbrs) remove_from_sol(v);
            add_to_sol(u);
            emit_move(comp_id, "probe", {u}, nbrs, cur_weight);
            improved = true;
        }
    }
    return improved;
}
static bool one_two_swap_pass(const vector<int>& comp, int comp_id) {
    bool improved = false;
    for (int v : comp) {
        if (!in_sol[v]) continue;
        vector<int> tight;
        for (int u : adj[v]) if (!removed[u] && !in_sol[u] && conf[u] == 1) tight.push_back(u);
        if (tight.size() < 2) continue;
        bool done = false;
        for (size_t i = 0; i < tight.size() && !done; i++)
            for (size_t k = i + 1; k < tight.size() && !done; k++) {
                int u1 = tight[i], u2 = tight[k];
                if (has_edge(u1, u2)) continue;
                if (W[u1] + W[u2] > W[v]) {
                    remove_from_sol(v); add_to_sol(u1); add_to_sol(u2);
                    emit_move(comp_id, "swap-1-2", {u1, u2}, {v}, cur_weight);
                    improved = true; done = true;
                }
            }
    }
    return improved;
}
static bool try_23_pair(int v1, int v2, int cand_limit, vector<int>& added) {
    vector<int> cands;
    for (int u : adj[v1]) if (!removed[u] && !in_sol[u] && conf[u] == 0) cands.push_back(u);
    for (int u : adj[v2]) if (!removed[u] && !in_sol[u] && conf[u] == 0) cands.push_back(u);
    sort(cands.begin(), cands.end());
    cands.erase(unique(cands.begin(), cands.end()), cands.end());
    sort(cands.begin(), cands.end(), [](int a, int b) { return W[a] > W[b]; });
    int lim = min((int)cands.size(), cand_limit);
    for (int p = 0; p < lim; p++) {
        int u1 = cands[p];
        if (conf[u1] > 0) continue;
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
                    added = {u1, u2, u3};
                    return true;
                }
            }
            remove_from_sol(u2);
        }
        remove_from_sol(u1);
    }
    return false;
}
static bool two_three_swap_pass(const vector<int>& comp, int comp_id) {
    bool improved = false;
    int cand_limit = ((int)comp.size() <= 300) ? 25 : 15;
    vector<int> sol_vec;
    for (int v : comp) if (in_sol[v]) sol_vec.push_back(v);
    int sz = (int)sol_vec.size();
    if (sz < 2) return false;
    for (int i = 0; i < sz && time_ok(); i++) {
        int v1 = sol_vec[i];
        if (!in_sol[v1]) continue;
        remove_from_sol(v1);
        bool done = false;
        for (int k = i + 1; k < sz && time_ok() && !done; k++) {
            int v2 = sol_vec[k];
            if (!in_sol[v2]) continue;
            remove_from_sol(v2);
            vector<int> added;
            if (try_23_pair(v1, v2, cand_limit, added)) {
                emit_move(comp_id, "swap-2-3", added, {v1, v2}, cur_weight);
                improved = true; done = true;
            } else add_to_sol(v2);
        }
        if (!done) add_to_sol(v1);
    }
    return improved;
}
static void local_search(const vector<int>& comp, int comp_id) {
    make_maximal(comp);
    bool improved = true;
    while (improved && time_ok()) {
        improved = false;
        if (probe_pass(comp, comp_id))                        improved = true;
        if (time_ok() && one_two_swap_pass(comp, comp_id))    improved = true;
        if (time_ok() && two_three_swap_pass(comp, comp_id))  improved = true;
    }
}

// global best bookkeeping so the UI can draw a live improvement curve
static ll  g_base_weight = 0;              // forced-in weight + fold offset
static vector<ll> g_comp_best;             // best weight found per component
static ll  g_last_total  = -1;
static void report_total(int comp_id, ll iter) {
    ll total = g_base_weight;
    for (ll x : g_comp_best) total += x;
    if (total == g_last_total) return;
    g_last_total = total;
    if (OPT.trace) {
        J j; j.str("e", "best").num("total", total).num("comp", comp_id).num("iter", iter);
        emit(j);
    }
}

static pair<ll, vector<int>> ils_component(const vector<int>& comp, int comp_id) {
    mt19937 rng(OPT.seed ^ (0x9E3779B9u * (unsigned)(comp_id + 1)));
    ll best_w = 0;
    vector<int> best_v;

    auto snapshot = [&]() {
        if (cur_weight <= best_w) return false;
        best_w = cur_weight;
        best_v.clear();
        for (int v : comp) if (in_sol[v]) best_v.push_back(v);
        g_comp_best[comp_id] = best_w;
        return true;
    };
    auto restore_best = [&]() {
        reset_sol(comp);
        for (int v : best_v) add_to_sol(v);
    };

    greedy_build(comp, rng, 0.0);
    if (OPT.trace) {
        vector<int> sel_now;
        for (int v : comp) if (in_sol[v]) sel_now.push_back(v);
        J j; j.str("e", "greedy").num("comp", comp_id).num("weight", cur_weight).arr("sel", sel_now)
             .str("msg", "GWMIN greedy on component " + to_string(comp_id) + " (" +
                         to_string(comp.size()) + " vertices): weight " + to_string(cur_weight) + ".");
        emit(j);
    }
    local_search(comp, comp_id);
    snapshot();
    report_total(comp_id, 0);

    double perturb_rate = 0.10;
    ll no_improve = 0, iter = 0;
    ll last_progress_ms = elapsed_ms();

    while (time_ok()) {
        if (OPT.max_iters > 0 && iter >= OPT.max_iters) break;
        if (OPT.stall > 0 && no_improve >= OPT.stall) break;

        restore_best();
        if (iter % 5 == 4) {
            greedy_build(comp, rng, 0.25);
        } else {
            int k = max(3, (int)(best_v.size() * perturb_rate));
            vector<int> sv;
            for (int v : comp) if (in_sol[v]) sv.push_back(v);
            shuffle(sv.begin(), sv.end(), rng);
            k = min(k, (int)sv.size());
            for (int i = 0; i < k; i++) remove_from_sol(sv[i]);
        }
        make_maximal(comp);
        local_search(comp, comp_id);

        if (snapshot()) {
            no_improve = 0;
            perturb_rate = 0.10;
            report_total(comp_id, iter);
            if (OPT.trace) {
                J j; j.str("e", "ils").num("comp", comp_id).num("iter", iter)
                     .num("weight", best_w).boo("improved", true).arr("sel", best_v)
                     .str("msg", "ILS iteration " + to_string(iter) + " improved component " +
                                 to_string(comp_id) + " to " + to_string(best_w) + ".");
                emit(j);
            }
        } else {
            no_improve++;
            if (no_improve % 40 == 0) perturb_rate = min(0.35, perturb_rate * 1.2);
        }
        iter++;

        ll now = elapsed_ms();
        if (OPT.trace && now - last_progress_ms >= 250) {
            last_progress_ms = now;
            J j; j.str("e", "progress").num("comp", comp_id).num("iter", iter)
                 .num("best", best_w).num("stall", no_improve);
            emit(j);
        }
    }
    if (OPT.trace) {
        J j; j.str("e", "ils_done").num("comp", comp_id).num("iters", iter).num("weight", best_w)
             .arr("sel", best_v)
             .str("msg", "Component " + to_string(comp_id) + " finished after " + to_string(iter) +
                         " ILS iterations at weight " + to_string(best_w) +
                         (g_stop ? " (stopped by operator)." : "."));
        emit(j);
    }
    return {best_w, best_v};
}

// ── reconstruction and verification ──────────────────────────────────────────
static vector<int> unfold(vector<int> sol) {
    vector<char> in_is(N + 1, 0);
    for (int v : sol) in_is[v] = 1;
    for (int i = (int)folds.size() - 1; i >= 0; i--) {
        const Fold& f = folds[i];
        if (f.type == 1) {
            if (!in_is[f.a]) { in_is[f.v] = 1; sol.push_back(f.v); }
            if (OPT.trace) {
                J j; j.str("e", "unfold").num("fold_id", i).str("type", "N-fold")
                     .num("v", f.v).num("a", f.a).boo("anchor_in", (bool)in_is[f.a])
                     .str("msg", string("N-fold ") + to_string(i) + ": v" + to_string(f.a) +
                                 (in_is[f.a] ? " is IN ⇒ v" : " is OUT ⇒ v") + to_string(f.v) +
                                 (in_is[f.a] ? " stays out." : " joins the set."));
                emit(j);
            }
        } else {
            bool a_in = in_is[f.a];
            if (a_in) { in_is[f.b] = 1; sol.push_back(f.b); }
            else      { in_is[f.v] = 1; sol.push_back(f.v); }
            if (OPT.trace) {
                J j; j.str("e", "unfold").num("fold_id", i).str("type", "V-fold")
                     .num("v", f.v).num("a", f.a).num("b", f.b).boo("anchor_in", a_in)
                     .str("msg", string("V-fold ") + to_string(i) + ": supernode v" + to_string(f.a) +
                                 (a_in ? " is IN ⇒ v" + to_string(f.b) + " joins."
                                       : " is OUT ⇒ v" + to_string(f.v) + " joins."));
                emit(j);
            }
        }
    }
    sort(sol.begin(), sol.end());
    sol.erase(unique(sol.begin(), sol.end()), sol.end());
    return sol;
}

struct VerifyResult { ll weight = 0; ll conflicts = 0; ll edges = 0; bool maximal = true; bool ok = false; };
static VerifyResult verify(const vector<int>& sol, const vector<pair<int,int>>& edges) {
    VerifyResult r;
    vector<char> in_is(N + 1, 0);
    for (int v : sol) in_is[v] = 1;
    for (auto& [u, v] : edges) { r.edges++; if (in_is[u] && in_is[v]) r.conflicts++; }
    for (int v : sol) r.weight += W0[v];
    // maximality: no vertex outside the set is free to join
    vector<char> blocked(N + 1, 0);
    for (int v : sol) for (int u : adj[v]) blocked[u] = 1;
    for (int v = 1; v <= N; v++) if (!in_is[v] && !blocked[v]) { r.maximal = false; break; }
    r.ok = (r.conflicts == 0);
    return r;
}

// ── main ─────────────────────────────────────────────────────────────────────
static void usage() {
    fprintf(stderr,
        "mwis_engine — Maximum Weight Independent Set solver with live tracing\n\n"
        "  --trace                 stream newline-delimited JSON events on stdout\n"
        "  --input FILE            read the instance from FILE instead of stdin\n"
        "  --time-limit SEC        wall-clock budget (default 0 = unlimited)\n"
        "  --stall N               stop a component after N non-improving ILS iterations\n"
        "                          (default 2000; 0 = never stop on its own)\n"
        "  --max-iters N           hard cap on ILS iterations per component (0 = unlimited)\n"
        "  --seed S                RNG seed (default 12345, runs are deterministic)\n"
        "  --exact-threshold N     certify components up to N vertices by branch & bound\n"
        "  --exact-nodes N         branch & bound node budget per component\n"
        "  --no-reduce             skip kernelization (ablation)\n"
        "  --legacy-rules          use solution.cpp's unsound R3/R4 variants (bug demo)\n"
        "  --disable RULE[,RULE]   switch rules off: R0 R1 R2 R3 R4 R5 R6 R7\n"
        "  --help\n");
}

int main(int argc, char** argv) {
    START_TIME = chrono::steady_clock::now();
    ios::sync_with_stdio(false);
    cin.tie(nullptr);
    signal(SIGTERM, on_signal);
    signal(SIGINT,  on_signal);

    for (int i = 1; i < argc; i++) {
        string a = argv[i];
        auto next = [&]() -> string { return (i + 1 < argc) ? string(argv[++i]) : string(); };
        if      (a == "--trace")           OPT.trace = true;
        else if (a == "--input")           OPT.input = next();
        else if (a == "--time-limit")      OPT.time_limit = atof(next().c_str());
        else if (a == "--stall")           OPT.stall = atoll(next().c_str());
        else if (a == "--max-iters")       OPT.max_iters = atoll(next().c_str());
        else if (a == "--seed")            OPT.seed = (unsigned)strtoul(next().c_str(), nullptr, 10);
        else if (a == "--exact-threshold") OPT.exact_threshold = atoi(next().c_str());
        else if (a == "--exact-nodes")     OPT.exact_nodes = atoll(next().c_str());
        else if (a == "--no-reduce")       OPT.no_reduce = true;
        else if (a == "--legacy-rules")    OPT.legacy_rules = true;
        else if (a == "--max-events")      OPT.max_search_events = atoi(next().c_str());
        else if (a == "--disable") {
            string list = next(), tok;
            stringstream ss(list);
            while (getline(ss, tok, ',')) if (!tok.empty()) OPT.disabled.insert(tok);
        }
        else if (a == "--help") { usage(); return 0; }
        else { fprintf(stderr, "unknown option: %s\n", a.c_str()); usage(); return 2; }
    }

    istream* in = &cin;
    ifstream fin;
    if (!OPT.input.empty()) {
        fin.open(OPT.input);
        if (!fin) { fprintf(stderr, "cannot open %s\n", OPT.input.c_str()); return 2; }
        in = &fin;
    }

    if (!((*in) >> N >> M)) { fprintf(stderr, "bad input: expected \"N M\"\n"); return 2; }
    if (N < 0 || M < 0) { fprintf(stderr, "bad input: negative sizes\n"); return 2; }

    W0.assign(N + 1, 0); W.assign(N + 1, 0);
    adj.assign(N + 1, {});
    removed.assign(N + 1, 0); forced_in.assign(N + 1, 0);
    live_deg.assign(N + 1, 0);
    in_sol.assign(N + 1, 0); conf.assign(N + 1, 0);

    for (int i = 1; i <= N; i++) { if (!((*in) >> W0[i])) { fprintf(stderr, "bad input: weights\n"); return 2; } W[i] = W0[i]; }
    vector<pair<int,int>> edges;
    edges.reserve(M);
    for (int i = 0; i < M; i++) {
        int u, v;
        if (!((*in) >> u >> v)) { fprintf(stderr, "bad input: edge %d\n", i); return 2; }
        if (u < 1 || u > N || v < 1 || v > N) { fprintf(stderr, "bad input: edge endpoint out of range\n"); return 2; }
        if (u == v) continue;
        adj[u].push_back(v);
        adj[v].push_back(u);
        edges.push_back({min(u,v), max(u,v)});
    }
    for (int i = 1; i <= N; i++) {
        sort(adj[i].begin(), adj[i].end());
        adj[i].erase(unique(adj[i].begin(), adj[i].end()), adj[i].end());
        live_deg[i] = (int)adj[i].size();
    }
    sort(edges.begin(), edges.end());
    edges.erase(unique(edges.begin(), edges.end()), edges.end());

    ll total_weight = 0;
    for (int i = 1; i <= N; i++) total_weight += W0[i];
    if (OPT.trace) {
        J j; j.str("e", "meta").num("n", N).num("m", (ll)edges.size()).num("total_weight", total_weight)
             .num("seed", OPT.seed).dbl("time_limit", OPT.time_limit).num("stall", OPT.stall)
             .num("exact_threshold", OPT.exact_threshold).boo("reduce", !OPT.no_reduce)
             .boo("legacy_rules", OPT.legacy_rules)
             .str("msg", "Instance: " + to_string(N) + " vertices, " + to_string(edges.size()) +
                         " edges, total weight " + to_string(total_weight) + ".");
        emit(j);
    }

    vector<int> kernel;
    if (OPT.no_reduce) {
        for (int v = 1; v <= N; v++) kernel.push_back(v);
        emit_msg("phase_start", "Kernelization disabled (--no-reduce): solving the raw instance.");
    } else {
        kernel = kernelize();
    }

    ll base_weight = fold_offset;
    vector<int> solution;
    for (int v = 1; v <= N; v++) if (forced_in[v]) { base_weight += W[v]; solution.push_back(v); }
    g_base_weight = base_weight;

    auto comps = get_components(kernel);
    sort(comps.begin(), comps.end(),
         [](const vector<int>& a, const vector<int>& b) { return a.size() < b.size(); });
    g_comp_best.assign(comps.size(), 0);

    if (OPT.trace) {
        vector<int> sizes;
        for (auto& c : comps) sizes.push_back((int)c.size());
        J j; j.str("e", "kernel").num("size", (ll)kernel.size()).num("components", (ll)comps.size())
             .arr("vertices", kernel).arr("component_sizes", sizes)
             .num("folds", (ll)folds.size()).num("offset", fold_offset).num("base", base_weight)
             .str("msg", "Kernel: " + to_string(kernel.size()) + " vertices in " +
                         to_string(comps.size()) + " component(s); " + to_string(folds.size()) +
                         " fold(s) recorded; decided weight so far " + to_string(base_weight) + ".");
        emit(j);
    }

    // Solve each component with the strongest method that fits it.
    vector<int> heuristic_comps;
    bool all_optimal = true;
    for (size_t ci = 0; ci < comps.size(); ci++) {
        auto& comp = comps[ci];
        vector<int> out;
        ll score = 0, nodes = 0;

        if (solve_tree(comp, out, score)) {
            g_comp_best[ci] = score;
            for (int v : out) solution.push_back(v);
            if (OPT.trace) {
                J j; j.str("e", "component").num("comp", (ll)ci).num("size", (ll)comp.size())
                     .str("method", "tree-dp").boo("optimal", true).num("weight", score)
                     .arr("vertices", comp).arr("selected", out)
                     .str("msg", "Component " + to_string(ci) + " (" + to_string(comp.size()) +
                                 " vertices) is a forest — exact O(n) DP gives " + to_string(score) + ".");
                emit(j);
            }
            report_total((int)ci, 0);
            continue;
        }
        if (solve_exact(comp, out, score, nodes)) {
            g_comp_best[ci] = score;
            for (int v : out) solution.push_back(v);
            if (OPT.trace) {
                J j; j.str("e", "component").num("comp", (ll)ci).num("size", (ll)comp.size())
                     .str("method", "branch-and-bound").boo("optimal", true).num("weight", score)
                     .num("nodes", nodes).arr("vertices", comp).arr("selected", out)
                     .str("msg", "Component " + to_string(ci) + " (" + to_string(comp.size()) +
                                 " vertices) solved exactly by branch & bound in " + to_string(nodes) +
                                 " nodes: " + to_string(score) + " (proven optimal).");
                emit(j);
            }
            report_total((int)ci, 0);
            continue;
        }
        heuristic_comps.push_back((int)ci);
        all_optimal = false;
        if (OPT.trace) {
            J j; j.str("e", "component").num("comp", (ll)ci).num("size", (ll)comp.size())
                 .str("method", "ils").boo("optimal", false).num("nodes", nodes)
                 .arr("vertices", comp)
                 .str("msg", "Component " + to_string(ci) + " (" + to_string(comp.size()) +
                             " vertices) exceeds the exact budget — handing it to ILS.");
            emit(j);
        }
    }

    for (int ci : heuristic_comps) {
        if (!time_ok()) break;
        auto [w, sel] = ils_component(comps[ci], ci);
        g_comp_best[ci] = w;
        for (int v : sel) solution.push_back(v);
    }

    ll kernel_weight = base_weight;
    for (ll x : g_comp_best) kernel_weight += x;

    solution = unfold(solution);
    VerifyResult vr = verify(solution, edges);
    bool accounting_ok = (vr.weight == kernel_weight);

    if (OPT.trace) {
        J j; j.str("e", "verify").num("weight", vr.weight).num("reported", kernel_weight)
             .num("conflicts", vr.conflicts).num("edges_checked", vr.edges)
             .num("size", (ll)solution.size()).boo("independent", vr.conflicts == 0)
             .boo("maximal", vr.maximal).boo("accounting_ok", accounting_ok)
             .boo("ok", vr.ok && accounting_ok)
             .str("msg", "Verification: " + to_string(vr.edges) + " edges checked, " +
                         to_string(vr.conflicts) + " conflict(s); weight recomputed from the "
                         "original weights is " + to_string(vr.weight) + " against " +
                         to_string(kernel_weight) + " tracked by the solver.");
        emit(j);

        vector<int> rc_names;
        string rules = "{";
        bool first = true;
        for (auto& [k, v] : rule_counts) {
            if (!first) rules += ',';
            first = false;
            rules += "\"" + jesc(k) + "\":" + to_string(v);
        }
        rules += "}";
        J d; d.str("e", "done").num("weight", vr.weight).num("size", (ll)solution.size())
             .arr("vertices", solution).boo("optimal", all_optimal && !g_stop)
             .boo("stopped", (bool)g_stop).boo("verified", vr.ok && accounting_ok)
             .num("kernel_size", (ll)kernel.size()).num("folds", (ll)folds.size())
             .raw("rule_counts", rules)
             .str("msg", string("Done: weight ") + to_string(vr.weight) + " on " +
                         to_string(solution.size()) + " vertices" +
                         (all_optimal && !g_stop ? ", proven optimal." : "."));
        emit(d);
    } else {
        printf("%lld\n", vr.weight);
        for (size_t i = 0; i < solution.size(); i++) printf(i ? " %d" : "%d", solution[i]);
        printf("\n");
        fflush(stdout);
    }

    if (!vr.ok || !accounting_ok) {
        fprintf(stderr, "VERIFICATION FAILED: conflicts=%lld reported=%lld actual=%lld\n",
                vr.conflicts, kernel_weight, vr.weight);
        return 1;
    }
    return 0;
}
