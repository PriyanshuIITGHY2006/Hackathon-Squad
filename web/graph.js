/* Canvas graph view: force layout, live per-vertex state, pan/zoom, hover.
   Kept dependency-free so the workbench runs with no network access. */
(function (global) {
  "use strict";

  const MAX_DRAW_N = 4000;      // beyond this a node-link drawing is unreadable
  const MAX_DRAW_M = 40000;

  function css(name, fallback) {
    const v = getComputedStyle(document.documentElement).getPropertyValue(name).trim();
    return v || fallback;
  }

  const GraphView = {
    canvas: null, ctx: null, tooltip: null, overlay: null, emptyEl: null,
    g: null,
    n: 0,
    x: null, y: null,
    state: null,              // Uint8Array: 0 live, 1 in, 2 out, 3 fold, 4 kernel
    touched: new Set(),
    radius: null,
    view: { scale: 1, tx: 0, ty: 0 },
    layoutIters: 0,
    layoutTarget: 0,
    running: false,
    labels: false,
    hover: -1,
    tooltipText: null,
    drawable: true,

    init(canvas, tooltip, overlay, emptyEl) {
      this.canvas = canvas;
      this.ctx = canvas.getContext("2d");
      this.tooltip = tooltip;
      this.overlay = overlay;
      this.emptyEl = emptyEl;
      this._bind();
      this._resize();
      new ResizeObserver(() => { this._resize(); this.draw(); }).observe(canvas.parentElement);
    },

    _bind() {
      const c = this.canvas;
      let dragging = false, lastX = 0, lastY = 0, moved = false;

      c.addEventListener("mousedown", (e) => { dragging = true; moved = false; lastX = e.clientX; lastY = e.clientY; });
      window.addEventListener("mouseup", () => { dragging = false; });
      window.addEventListener("mousemove", (e) => {
        if (dragging) {
          moved = true;
          this.view.tx += e.clientX - lastX;
          this.view.ty += e.clientY - lastY;
          lastX = e.clientX; lastY = e.clientY;
          this.draw();
        }
      });
      c.addEventListener("mousemove", (e) => {
        const r = c.getBoundingClientRect();
        this._hoverAt(e.clientX - r.left, e.clientY - r.top, e.clientX - r.left, e.clientY - r.top);
      });
      c.addEventListener("mouseleave", () => { this.hover = -1; this.tooltip.hidden = true; this.draw(); });
      c.addEventListener("wheel", (e) => {
        e.preventDefault();
        const r = c.getBoundingClientRect();
        const mx = e.clientX - r.left, my = e.clientY - r.top;
        const k = Math.exp(-e.deltaY * 0.0014);
        const ns = Math.max(0.12, Math.min(9, this.view.scale * k));
        const ratio = ns / this.view.scale;
        this.view.tx = mx - (mx - this.view.tx) * ratio;
        this.view.ty = my - (my - this.view.ty) * ratio;
        this.view.scale = ns;
        this.draw();
      }, { passive: false });
    },

    _resize() {
      const dpr = Math.min(window.devicePixelRatio || 1, 2);
      const rect = this.canvas.parentElement.getBoundingClientRect();
      this.canvas.width = Math.max(1, Math.floor(rect.width * dpr));
      this.canvas.height = Math.max(1, Math.floor(rect.height * dpr));
      this.dpr = dpr;
      this.w = rect.width;
      this.h = rect.height;
    },

    setGraph(g) {
      this.g = g;
      this.n = g.n;
      this.touched.clear();
      this.state = new Uint8Array(g.n + 1);
      this.drawable = g.n <= MAX_DRAW_N && g.edges.length <= MAX_DRAW_M;
      this.hover = -1;
      this.tooltip.hidden = true;

      const maxW = Math.max(1, ...g.weights);
      this.radius = new Float32Array(g.n + 1);
      const base = g.n > 1200 ? 1.8 : g.n > 600 ? 2.4 : g.n > 200 ? 3.0
                 : g.n > 90 ? 4.2 : g.n > 30 ? 7 : 10;
      for (let i = 1; i <= g.n; i++) {
        this.radius[i] = base * (0.62 + 0.75 * Math.sqrt(g.weights[i - 1] / maxW));
      }

      // adjacency in flat arrays for fast layout passes
      const deg = new Int32Array(g.n + 2);
      for (const [u, v] of g.edges) { deg[u]++; deg[v]++; }
      this.off = new Int32Array(g.n + 2);
      for (let i = 1; i <= g.n; i++) this.off[i + 1] = this.off[i] + deg[i];
      this.nbr = new Int32Array(this.off[g.n + 1]);
      const cur = this.off.slice();
      for (const [u, v] of g.edges) { this.nbr[cur[u]++] = v; this.nbr[cur[v]++] = u; }
      this.deg = deg;

      // deterministic starting positions (same instance ⇒ same picture)
      let s = (g.seed || 1) >>> 0 || 1;
      const rnd = () => { s ^= s << 13; s >>>= 0; s ^= s >> 17; s ^= s << 5; s >>>= 0; return s / 4294967296; };
      this.x = new Float32Array(g.n + 1);
      this.y = new Float32Array(g.n + 1);
      const R = Math.min(this.w, this.h) * 0.42 || 260;
      for (let i = 1; i <= g.n; i++) {
        const a = rnd() * Math.PI * 2, rr = Math.sqrt(rnd()) * R;
        this.x[i] = Math.cos(a) * rr;
        this.y[i] = Math.sin(a) * rr;
      }
      this.layoutIters = 0;
      this.layoutTarget = g.n > 1500 ? 140 : g.n > 400 ? 260 : 360;
      this.view = { scale: 1, tx: this.w / 2, ty: this.h / 2 };
      this.emptyEl.hidden = true;
      this.startLayout();
    },

    setStates(arr) { this.state = arr; },

    markTouched(ids) {
      this.touched.clear();
      if (ids) for (const id of ids) this.touched.add(id);
    },

    startLayout() {
      if (this.running || !this.drawable) { this.draw(); return; }
      this.running = true;
      const step = () => {
        const budget = performance.now() + 12;
        while (this.layoutIters < this.layoutTarget && performance.now() < budget) {
          this._layoutStep();
          this.layoutIters++;
        }
        this.draw();
        if (this.layoutIters < this.layoutTarget) requestAnimationFrame(step);
        else { this.running = false; this.fit(); }
      };
      requestAnimationFrame(step);
    },

    /* One Fruchterman-Reingold pass. Repulsion is limited to a spatial-grid
       neighbourhood so the cost stays near-linear for larger instances. */
    _layoutStep() {
      const n = this.n, x = this.x, y = this.y;
      if (n < 2) return;
      const area = 620 * 620;
      const k = Math.sqrt(area / n) * 1.05;
      const t = Math.max(1.5, 42 * (1 - this.layoutIters / this.layoutTarget));
      const dx = new Float32Array(n + 1), dy = new Float32Array(n + 1);

      const cell = k * 1.8;
      const buckets = new Map();
      const key = (i) => (Math.floor(x[i] / cell) * 73856093) ^ (Math.floor(y[i] / cell) * 19349663);
      for (let i = 1; i <= n; i++) {
        const kk = key(i);
        let b = buckets.get(kk);
        if (!b) buckets.set(kk, (b = []));
        b.push(i);
      }
      for (let i = 1; i <= n; i++) {
        const cx = Math.floor(x[i] / cell), cy = Math.floor(y[i] / cell);
        for (let a = -1; a <= 1; a++) for (let b = -1; b <= 1; b++) {
          const bucket = buckets.get(((cx + a) * 73856093) ^ ((cy + b) * 19349663));
          if (!bucket) continue;
          for (const j of bucket) {
            if (j === i) continue;
            let ux = x[i] - x[j], uy = y[i] - y[j];
            let d2 = ux * ux + uy * uy;
            if (d2 < 1e-4) { ux = (i - j) * 0.01 + 0.01; uy = 0.01; d2 = 1e-4; }
            const d = Math.sqrt(d2);
            if (d > cell * 2) continue;
            const f = (k * k) / d;
            dx[i] += (ux / d) * f; dy[i] += (uy / d) * f;
          }
        }
      }
      for (let u = 1; u <= n; u++) {
        for (let p = this.off[u]; p < this.off[u + 1]; p++) {
          const v = this.nbr[p];
          if (v < u) continue;
          let ux = x[u] - x[v], uy = y[u] - y[v];
          let d = Math.sqrt(ux * ux + uy * uy) || 0.01;
          const f = (d * d) / k;
          const fx = (ux / d) * f, fy = (uy / d) * f;
          dx[u] -= fx; dy[u] -= fy;
          dx[v] += fx; dy[v] += fy;
        }
      }
      for (let i = 1; i <= n; i++) {
        // mild gravity keeps disconnected pieces from drifting off screen
        dx[i] -= x[i] * 0.012;
        dy[i] -= y[i] * 0.012;
        const d = Math.sqrt(dx[i] * dx[i] + dy[i] * dy[i]) || 1;
        const lim = Math.min(d, t);
        x[i] += (dx[i] / d) * lim;
        y[i] += (dy[i] / d) * lim;
      }
    },

    fit() {
      if (!this.g || this.n === 0) return;
      let minX = Infinity, maxX = -Infinity, minY = Infinity, maxY = -Infinity;
      for (let i = 1; i <= this.n; i++) {
        if (this.x[i] < minX) minX = this.x[i];
        if (this.x[i] > maxX) maxX = this.x[i];
        if (this.y[i] < minY) minY = this.y[i];
        if (this.y[i] > maxY) maxY = this.y[i];
      }
      const pad = 34;
      const sx = (this.w - pad * 2) / Math.max(1, maxX - minX);
      const sy = (this.h - pad * 2) / Math.max(1, maxY - minY);
      // cap the zoom so a small or tightly-packed layout does not blow the
      // vertices up into overlapping blobs
      this.view.scale = Math.max(0.12, Math.min(1.5, Math.min(sx, sy)));
      this.view.tx = this.w / 2 - ((minX + maxX) / 2) * this.view.scale;
      this.view.ty = this.h / 2 - ((minY + maxY) / 2) * this.view.scale;
      this.draw();
    },

    _hoverAt(px, py) {
      if (!this.g || !this.drawable) return;
      const s = this.view.scale;
      const gx = (px - this.view.tx) / s, gy = (py - this.view.ty) / s;
      let best = -1, bestD = Infinity;
      for (let i = 1; i <= this.n; i++) {
        const dx = this.x[i] - gx, dy = this.y[i] - gy;
        const d = dx * dx + dy * dy;
        const rr = (this.radius[i] + 4) * (this.radius[i] + 4);
        if (d < rr && d < bestD) { bestD = d; best = i; }
      }
      if (best !== this.hover) { this.hover = best; this.draw(); }
      if (best > 0) {
        const names = ["live", "in the set", "excluded", "folded away", "hard kernel"];
        const d = this.deg ? this.deg[best] : 0;
        this.tooltip.hidden = false;
        this.tooltip.textContent =
          "v" + best + "\nweight  " + this.g.weights[best - 1] +
          "\ndegree  " + d + "\nstate   " + names[this.state[best] || 0];
        const tw = this.tooltip.offsetWidth, th = this.tooltip.offsetHeight;
        this.tooltip.style.left = Math.min(this.w - tw - 6, px + 14) + "px";
        this.tooltip.style.top = Math.max(4, Math.min(this.h - th - 6, py - th - 10)) + "px";
      } else {
        this.tooltip.hidden = true;
      }
    },

    setOverlay(text) { if (this.overlay) this.overlay.textContent = text || ""; },

    draw() {
      const ctx = this.ctx;
      if (!ctx) return;
      ctx.save();
      ctx.setTransform(this.dpr, 0, 0, this.dpr, 0, 0);
      ctx.clearRect(0, 0, this.w, this.h);
      if (!this.g) { ctx.restore(); return; }

      if (!this.drawable) {
        ctx.restore();
        this.emptyEl.hidden = false;
        this.emptyEl.textContent =
          "Instance too large to draw as a node-link diagram (" + this.n + " vertices, " +
          this.g.edges.length + " edges). The solve still runs — watch the metrics, " +
          "progress chart and trace below.";
        return;
      }
      this.emptyEl.hidden = true;

      const C = {
        edge: css("--border-hi", "#31424f"),
        live: css("--faint", "#5f7084"),
        inSet: css("--in", "#16b9c6"),
        out: css("--out", "#5a6b7c"),
        fold: css("--fold", "#a98bf5"),
        kernel: css("--ink", "#e5edf5"),
        touch: css("--touch", "#e2973c"),
        panel: css("--panel", "#121a22"),
      };
      const s = this.view.scale;
      ctx.translate(this.view.tx, this.view.ty);
      ctx.scale(s, s);

      // edges — drawn in two passes so live structure reads above decided edges
      const edges = this.g.edges;
      ctx.lineWidth = Math.max(0.35, 1 / s);
      ctx.strokeStyle = C.edge;
      ctx.globalAlpha = 0.34;
      ctx.beginPath();
      for (let e = 0; e < edges.length; e++) {
        const u = edges[e][0], v = edges[e][1];
        const su = this.state[u], sv = this.state[v];
        if (su === 0 || su === 4 || sv === 0 || sv === 4) continue;
        ctx.moveTo(this.x[u], this.y[u]);
        ctx.lineTo(this.x[v], this.y[v]);
      }
      ctx.stroke();
      ctx.globalAlpha = 0.8;
      ctx.beginPath();
      for (let e = 0; e < edges.length; e++) {
        const u = edges[e][0], v = edges[e][1];
        const su = this.state[u], sv = this.state[v];
        if (!(su === 0 || su === 4 || sv === 0 || sv === 4)) continue;
        ctx.moveTo(this.x[u], this.y[u]);
        ctx.lineTo(this.x[v], this.y[v]);
      }
      ctx.stroke();
      ctx.globalAlpha = 1;

      // vertices
      const showLabels = this.labels && this.n <= 320 && s > 0.55;
      for (let i = 1; i <= this.n; i++) {
        const st = this.state[i];
        const r = this.radius[i];
        ctx.beginPath();
        ctx.arc(this.x[i], this.y[i], r, 0, Math.PI * 2);
        if (st === 1) { ctx.fillStyle = C.inSet; ctx.fill(); ctx.strokeStyle = C.inSet; }
        else if (st === 2) { ctx.globalAlpha = 0.34; ctx.fillStyle = C.panel; ctx.fill(); ctx.globalAlpha = 1; ctx.strokeStyle = C.out; }
        else if (st === 3) { ctx.fillStyle = C.panel; ctx.fill(); ctx.strokeStyle = C.fold; }
        else if (st === 4) { ctx.fillStyle = C.panel; ctx.fill(); ctx.strokeStyle = C.kernel; }
        else { ctx.fillStyle = C.panel; ctx.fill(); ctx.strokeStyle = C.live; }
        ctx.lineWidth = Math.max(0.6, (st === 4 ? 2.1 : 1.5) / Math.max(s, 0.5));
        if (st === 3) ctx.setLineDash([3 / s, 2 / s]);
        ctx.stroke();
        ctx.setLineDash([]);

        if (this.touched.has(i)) {
          ctx.beginPath();
          ctx.arc(this.x[i], this.y[i], r + 3.5 / Math.max(s, 0.6), 0, Math.PI * 2);
          ctx.strokeStyle = C.touch;
          ctx.lineWidth = Math.max(1, 2.2 / Math.max(s, 0.6));
          ctx.stroke();
        }
        if (i === this.hover) {
          ctx.beginPath();
          ctx.arc(this.x[i], this.y[i], r + 6 / Math.max(s, 0.6), 0, Math.PI * 2);
          ctx.strokeStyle = C.kernel;
          ctx.lineWidth = Math.max(0.8, 1.2 / Math.max(s, 0.6));
          ctx.stroke();
        }
        if (showLabels) {
          ctx.fillStyle = st === 1 ? "#04181b" : C.live;
          ctx.font = (Math.max(7, r * 0.95)) + "px ui-monospace, monospace";
          ctx.textAlign = "center";
          ctx.textBaseline = "middle";
          ctx.fillText(String(this.g.weights[i - 1]), this.x[i], this.y[i]);
        }
      }
      ctx.restore();
    },
  };

  global.GraphView = GraphView;
})(window);
