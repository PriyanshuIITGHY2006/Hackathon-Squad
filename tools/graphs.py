"""Graph generators shared by the web backend and the correctness harness.

Every generator returns the same dictionary:

    {"n": int, "weights": [w1..wn], "edges": [[u, v], ...]}

Vertices are 1-indexed to match the engine's input format.  Generation is fully
determined by (kind, n, seed, density), so any instance the website shows can be
reproduced offline from the three values printed next to it.
"""

import random

KINDS = [
    "random", "tree", "path", "cycle", "star", "complete", "bipartite",
    "cliques", "matching", "grid", "geometric", "scalefree", "hard-dense",
]


def _weights(rng, n, wmax):
    return [rng.randint(1, wmax) for _ in range(n)]


def generate(kind="random", n=40, seed=1, density=0.12, wmax=100):
    """Build an instance. `density` is the edge probability where it applies."""
    rng = random.Random(seed)
    n = max(1, int(n))
    edges = set()

    def add(u, v):
        if u != v:
            edges.add((min(u, v), max(u, v)))

    if kind == "path":
        for i in range(1, n):
            add(i, i + 1)

    elif kind == "cycle":
        for i in range(1, n):
            add(i, i + 1)
        if n > 2:
            add(n, 1)

    elif kind == "star":
        for i in range(2, n + 1):
            add(1, i)

    elif kind == "complete":
        for i in range(1, n + 1):
            for j in range(i + 1, n + 1):
                add(i, j)

    elif kind == "tree":
        for i in range(2, n + 1):
            add(i, rng.randint(1, i - 1))

    elif kind == "bipartite":
        h = max(1, n // 2)
        for i in range(1, h + 1):
            for j in range(h + 1, n + 1):
                if rng.random() < max(density, 0.05):
                    add(i, j)

    elif kind == "cliques":
        i = 1
        while i <= n:
            size = min(rng.randint(3, 6), n - i + 1)
            for a in range(i, i + size):
                for b in range(a + 1, i + size):
                    add(a, b)
            i += size

    elif kind == "matching":
        for i in range(1, n, 2):
            if i + 1 <= n:
                add(i, i + 1)

    elif kind == "grid":
        cols = max(1, int(n ** 0.5))
        rows = (n + cols - 1) // cols
        for r in range(rows):
            for c in range(cols):
                v = r * cols + c + 1
                if v > n:
                    continue
                if c + 1 < cols and v + 1 <= n:
                    add(v, v + 1)
                if r + 1 < rows and v + cols <= n:
                    add(v, v + cols)

    elif kind == "geometric":
        # unit-disk graph: vertices in the unit square, edges within a radius
        # chosen so the expected degree tracks `density`
        pts = [(rng.random(), rng.random()) for _ in range(n)]
        target_deg = max(2.0, density * n)
        radius = (target_deg / (3.14159 * max(n - 1, 1))) ** 0.5
        for i in range(n):
            for j in range(i + 1, n):
                dx = pts[i][0] - pts[j][0]
                dy = pts[i][1] - pts[j][1]
                if dx * dx + dy * dy <= radius * radius:
                    add(i + 1, j + 1)

    elif kind == "scalefree":
        # Barabasi-Albert preferential attachment: hub-heavy, like real networks
        m = max(1, min(3, n - 1))
        targets = list(range(1, m + 1))
        repeated = list(targets)
        for v in range(m + 1, n + 1):
            chosen = set()
            while len(chosen) < min(m, len(set(repeated))):
                chosen.add(rng.choice(repeated))
            for t in chosen:
                add(v, t)
                repeated.append(t)
            repeated.extend([v] * len(chosen))

    elif kind == "hard-dense":
        # dense random graphs are where kernelization fails and search matters
        p = max(density, 0.5)
        for i in range(1, n + 1):
            for j in range(i + 1, n + 1):
                if rng.random() < p:
                    add(i, j)

    else:  # "random"
        for i in range(1, n + 1):
            for j in range(i + 1, n + 1):
                if rng.random() < density:
                    add(i, j)

    return {
        "n": n,
        "weights": _weights(rng, n, wmax),
        "edges": sorted(edges),
        "kind": kind,
        "seed": seed,
    }


def to_engine_text(g):
    """Serialize to the engine's stdin format."""
    lines = ["%d %d" % (g["n"], len(g["edges"]))]
    lines.append(" ".join(str(w) for w in g["weights"]))
    lines.extend("%d %d" % (u, v) for u, v in g["edges"])
    return "\n".join(lines) + "\n"


def parse_engine_text(text):
    """Read the engine input format back into a graph dict."""
    toks = text.split()
    pos = 0
    n = int(toks[pos]); pos += 1
    m = int(toks[pos]); pos += 1
    weights = []
    for _ in range(n):
        weights.append(int(toks[pos])); pos += 1
    edges = []
    for _ in range(m):
        u = int(toks[pos]); pos += 1
        v = int(toks[pos]); pos += 1
        if u != v:
            edges.append((min(u, v), max(u, v)))
    return {"n": n, "weights": weights, "edges": sorted(set(edges)),
            "kind": "custom", "seed": 0}


def verify(g, vertices):
    """Independently check a claimed solution. Returns (ok, weight, conflicts)."""
    sel = set(vertices)
    for v in sel:
        if v < 1 or v > g["n"]:
            return False, 0, -1
    conflicts = sum(1 for u, v in g["edges"] if u in sel and v in sel)
    weight = sum(g["weights"][v - 1] for v in sel)
    return conflicts == 0, weight, conflicts
