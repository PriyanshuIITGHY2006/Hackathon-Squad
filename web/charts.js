/* Live "best weight over time" chart. Plain canvas, no dependencies. */
(function (global) {
  "use strict";

  function css(name, fallback) {
    const v = getComputedStyle(document.documentElement).getPropertyValue(name).trim();
    return v || fallback;
  }

  const Chart = {
    canvas: null, ctx: null,
    pts: [],            // [ms, weight]
    reference: null,    // {value, label} — e.g. a proven optimum
    dpr: 1, w: 0, h: 0,

    init(canvas) {
      this.canvas = canvas;
      this.ctx = canvas.getContext("2d");
      this._resize();
      new ResizeObserver(() => { this._resize(); this.draw(); }).observe(canvas);
    },
    _resize() {
      const dpr = Math.min(window.devicePixelRatio || 1, 2);
      const rect = this.canvas.getBoundingClientRect();
      this.canvas.width = Math.max(1, Math.floor(rect.width * dpr));
      this.canvas.height = Math.max(1, Math.floor(rect.height * dpr));
      this.dpr = dpr; this.w = rect.width; this.h = rect.height;
    },
    reset() { this.pts = []; this.reference = null; this.draw(); },
    setReference(value, label) { this.reference = { value, label }; this.draw(); },
    push(ms, weight) {
      const last = this.pts[this.pts.length - 1];
      if (last && last[0] === ms && last[1] === weight) return;
      this.pts.push([ms, weight]);
      if (this.pts.length > 4000) this.pts = this.pts.filter((_, i) => i % 2 === 0 || i > this.pts.length - 500);
    },

    draw() {
      const ctx = this.ctx;
      if (!ctx) return;
      ctx.setTransform(this.dpr, 0, 0, this.dpr, 0, 0);
      ctx.clearRect(0, 0, this.w, this.h);

      const ink = css("--ink", "#e5edf5");
      const faint = css("--faint", "#5f7084");
      const line = css("--in", "#16b9c6");
      const grid = css("--border", "#22303d");
      const touch = css("--touch", "#e2973c");

      const padL = 8, padR = 10, padT = 12, padB = 16;
      const W = this.w - padL - padR, H = this.h - padT - padB;

      ctx.strokeStyle = grid;
      ctx.lineWidth = 1;
      ctx.globalAlpha = 0.65;
      for (let i = 0; i <= 3; i++) {
        const y = padT + (H * i) / 3;
        ctx.beginPath(); ctx.moveTo(padL, y); ctx.lineTo(padL + W, y); ctx.stroke();
      }
      ctx.globalAlpha = 1;

      if (this.pts.length === 0) {
        ctx.fillStyle = faint;
        ctx.font = "11px ui-monospace, monospace";
        ctx.textAlign = "center";
        ctx.fillText("waiting for the first solution…", this.w / 2, this.h / 2);
        return;
      }

      let maxT = 1, minW = Infinity, maxW = -Infinity;
      for (const [t, w] of this.pts) {
        if (t > maxT) maxT = t;
        if (w < minW) minW = w;
        if (w > maxW) maxW = w;
      }
      if (this.reference) { maxW = Math.max(maxW, this.reference.value); minW = Math.min(minW, this.reference.value); }
      const span = Math.max(1, maxW - minW);
      const lo = minW - span * 0.12, hi = maxW + span * 0.12;
      const X = (t) => padL + (W * t) / maxT;
      const Y = (w) => padT + H - (H * (w - lo)) / (hi - lo);

      if (this.reference) {
        const y = Y(this.reference.value);
        ctx.strokeStyle = touch;
        ctx.setLineDash([4, 3]);
        ctx.beginPath(); ctx.moveTo(padL, y); ctx.lineTo(padL + W, y); ctx.stroke();
        ctx.setLineDash([]);
        ctx.fillStyle = touch;
        ctx.font = "10px ui-monospace, monospace";
        ctx.textAlign = "left";
        ctx.fillText(this.reference.label || "optimum", padL + 2, Math.max(9, y - 4));
      }

      // step line: the best weight only ever moves up, and holds between jumps
      ctx.beginPath();
      ctx.moveTo(X(this.pts[0][0]), Y(this.pts[0][1]));
      for (let i = 1; i < this.pts.length; i++) {
        ctx.lineTo(X(this.pts[i][0]), Y(this.pts[i - 1][1]));
        ctx.lineTo(X(this.pts[i][0]), Y(this.pts[i][1]));
      }
      const lastPt = this.pts[this.pts.length - 1];
      ctx.lineTo(X(maxT), Y(lastPt[1]));
      ctx.strokeStyle = line;
      ctx.lineWidth = 1.6;
      ctx.lineJoin = "round";
      ctx.stroke();

      ctx.lineTo(X(maxT), padT + H);
      ctx.lineTo(X(this.pts[0][0]), padT + H);
      ctx.closePath();
      ctx.globalAlpha = 0.14;
      ctx.fillStyle = line;
      ctx.fill();
      ctx.globalAlpha = 1;

      ctx.beginPath();
      ctx.arc(X(maxT), Y(lastPt[1]), 2.8, 0, Math.PI * 2);
      ctx.fillStyle = line;
      ctx.fill();

      ctx.fillStyle = ink;
      ctx.font = "11px ui-monospace, monospace";
      ctx.textAlign = "left";
      ctx.fillText(String(lastPt[1]), padL + 2, padT - 2);
      ctx.fillStyle = faint;
      ctx.textAlign = "right";
      ctx.fillText((maxT / 1000).toFixed(2) + " s", padL + W, this.h - 4);
    },
  };

  global.Chart = Chart;
})(window);
