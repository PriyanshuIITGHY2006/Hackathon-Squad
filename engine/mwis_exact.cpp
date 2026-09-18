// ─────────────────────────────────────────────────────────────────────────────
//  mwis_exact — independent exact MWIS solver, used to certify the engine.
//
//  Deliberately implemented with a *different* algorithm from the engine's
//  branch & bound (which branches on the maximum-degree vertex and prunes with
//  a greedy clique cover).  This one walks vertices in index order and prunes
//  with a suffix-weight bound.  Two independent implementations agreeing on
//  thousands of instances is evidence; one implementation agreeing with itself
//  is not.
//
//  No reductions, no heuristics — just search.  Practical to n ≈ 60 on sparse
//  graphs, which is all the correctness harness needs.
//
//  Input:  same format as mwis_engine (N M / weights / edges).
//  Output: total weight, then the sorted vertex list.
// ─────────────────────────────────────────────────────────────────────────────
#include <bits/stdc++.h>
using namespace std;
using ll = long long;

static int N, M;
static vector<ll> W;
static vector<unsigned long long> nb;   // adjacency bitmask (n <= 64)
static ll best;
static unsigned long long best_set;
static vector<ll> suffix;               // suffix[i] = w[i] + w[i+1] + ... + w[n-1]
static ll nodes = 0;

static void rec(int i, unsigned long long banned, ll cur, unsigned long long chosen) {
    nodes++;
    if (cur > best) { best = cur; best_set = chosen; }
    if (i >= N) return;
    if (cur + suffix[i] <= best) return;            // no remaining vertex can help

    // branch 1: take vertex i when it is still allowed
    if (!((banned >> i) & 1ULL))
        rec(i + 1, banned | nb[i] | (1ULL << i), cur + W[i], chosen | (1ULL << i));

    // branch 2: skip vertex i
    rec(i + 1, banned, cur, chosen);
}

int main(int argc, char** argv) {
    ios::sync_with_stdio(false);
    cin.tie(nullptr);

    istream* in = &cin;
    ifstream fin;
    for (int i = 1; i < argc; i++) {
        string a = argv[i];
        if (a == "--input" && i + 1 < argc) {
            fin.open(argv[++i]);
            if (!fin) { fprintf(stderr, "cannot open input\n"); return 2; }
            in = &fin;
        }
    }

    if (!((*in) >> N >> M)) { fprintf(stderr, "bad input\n"); return 2; }
    if (N > 64) { fprintf(stderr, "mwis_exact supports at most 64 vertices (got %d)\n", N); return 3; }

    W.assign(N, 0);
    nb.assign(N, 0ULL);
    for (int i = 0; i < N; i++) (*in) >> W[i];
    for (int i = 0; i < M; i++) {
        int u, v; (*in) >> u >> v;
        u--; v--;
        if (u == v || u < 0 || v < 0 || u >= N || v >= N) continue;
        nb[u] |= (1ULL << v);
        nb[v] |= (1ULL << u);
    }

    suffix.assign(N + 1, 0);
    for (int i = N - 1; i >= 0; i--) suffix[i] = suffix[i + 1] + W[i];

    best = 0; best_set = 0ULL;
    rec(0, 0ULL, 0, 0ULL);

    printf("%lld\n", best);
    bool first = true;
    for (int i = 0; i < N; i++)
        if ((best_set >> i) & 1ULL) { printf(first ? "%d" : " %d", i + 1); first = false; }
    printf("\n");
    fprintf(stderr, "nodes=%lld\n", nodes);
    return 0;
}
