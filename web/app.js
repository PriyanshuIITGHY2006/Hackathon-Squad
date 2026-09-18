/* MWIS Workbench — talks to the C++ engine over SSE and drives every panel. */
(function () {
  "use strict";

  const S_LIVE = 0, S_IN = 1, S_OUT = 2, S_FOLD = 3, S_KERNEL = 4;
  const MAX_TRACE_ROWS = 1500;
  const MAX_CONSOLE_LINES = 1200;

  const $ = (id) => document.getElementById(id);
  const el = {
    kind: $("kind"), nodes: $("nodes"), nodesOut: $("nodesOut"),
    density: $("density"), densityOut: $("densityOut"),
    seed: $("seed"), wmax: $("wmax"),
    genBtn: $("genBtn"), pasteBtn: $("pasteBtn"),
    engSeed: $("engSeed"), stall: $("stall"),
    exactThreshold: $("exactThreshold"), timeLimit: $("timeLimit"),
    legacyRules: $("legacyRules"),
    runBtn: $("runBtn"), stopBtn: $("stopBtn"),
    compareBtn: $("compareBtn"), exactBtn: $("exactBtn"),
    buildPill: $("buildPill"), runPill: $("runPill"), clockPill: $("clockPill"),
    cmdtext: $("cmdtext"), graphMeta: $("graphMeta"),
    traceList: $("traceList"), consoleOut: $("consoleOut"),
    ledger: $("ledger"), correctness: $("correctness"),
    components: $("components"), followTail: $("followTail"), clearLog: $("clearLog"),
    fitBtn: $("fitBtn"), labelsBtn: $("labelsBtn"),
    pasteDialog: $("pasteDialog"), pasteText: $("pasteText"),
    mBest: $("mBest"), mN: $("mN"), mM: $("mM"), mLive: $("mLive"),
    mKernel: $("mKernel"), mFolds: $("mFolds"), mEvents: $("mEvents"),
  };

  const app = {
    graph: null,
    states: null,
    run: null,            // {id, es, startedAt}
    eventCount: 0,
    best: 0,
    compVerts: {},        // component id -> vertex list
    pending: [],          // events buffered between frames
    consoleLines: [],
    traceQueue: [],
    dirty: false,
    finished: true,
  };

  const RULE_NAMES = {
    R0: "isolated vertex", R1: "degree-1 include", R1F: "degree-1 N-fold",
    R2: "neighbourhood removal", R3: "simplicial vertex", R4: "domination",
    R5: "degree-2 V-fold", R6: "weighted twins",
    "R7-in": "LP forces in", "R7-out": "LP forces out",
    "R1-include": "degree-1 include", "R1-fold": "degree-1 N-fold",
    "R3-legacy": "triangle (unsound variant)", "R4-legacy": "domination (unsound variant)",
  };

  // ── helpers ────────────────────────────────────────────────────────────────
  function fmt(n) { return (n === null || n === undefined) ? "—" : String(n).replace(/\B(?=(\d{3})+(?!\d))/g, " "); }

  function setPill(pill, text, state) { pill.textContent = text; pill.dataset.state = state || "idle"; }

  function parseEngineText(text) {
    const t = text.trim().split(/\s+/).map(Number);
    let p = 0;
    const n = t[p++], m = t[p++];
    if (!Number.isFinite(n) || !Number.isFinite(m)) throw new Error("expected \"N M\" on the first line");
    const weights = [];
    for (let i = 0; i < n; i++) weights.push(t[p++]);
    const edges = [];
    for (let i = 0; i < m; i++) {
      const u = t[p++], v = t[p++];
      if (u !== v) edges.push([Math.min(u, v), Math.max(u, v)]);
    }
    if (weights.some((w) => !Number.isFinite(w))) throw new Error("weight list is short or malformed");
    return { n, weights, edges, kind: "custom", seed: 1 };
  }

  async function api(path, body) {
    const res = await fetch(path, {
      method: body ? "POST" : "GET",
      headers: body ? { "Content-Type": "application/json" } : undefined,
      body: body ? JSON.stringify(body) : undefined,
    });
    const data = await res.json().catch(() => ({ error: "bad response" }));
    if (!res.ok) throw new Error(data.error || ("HTTP " + res.status));
    return data;
  }

  // ── rendering ──────────────────────────────────────────────────────────────
  function markDirty() {
    if (app.dirty) return;
    app.dirty = true;
    requestAnimationFrame(flush);
  }

  function flush() {
    app.dirty = false;
    if (app.traceQueue.length) {
      const frag = document.createDocumentFragment();
      for (const row of app.traceQueue) frag.appendChild(row);
      app.traceQueue.length = 0;
      const emptyMsg = el.traceList.querySelector(".empty");
      if (emptyMsg) emptyMsg.remove();
      el.traceList.appendChild(frag);
      while (el.traceList.children.length > MAX_TRACE_ROWS) el.traceList.removeChild(el.traceList.firstChild);
      if (el.followTail.checked) el.traceList.scrollTop = el.traceList.scrollHeight;
    }
    if (app.consoleDirty) {
      app.consoleDirty = false;
      el.consoleOut.textContent = app.consoleLines.join("\n");
      if (el.followTail.checked) el.consoleOut.parentElement.scrollTop = el.consoleOut.parentElement.scrollHeight;
    }
    GraphView.draw();
    Chart.draw();
    updateMetrics();
  }

  function updateMetrics() {
    if (!app.graph) return;
    let live = 0;
    for (let i = 1; i <= app.graph.n; i++) if (app.states[i] === S_LIVE || app.states[i] === S_KERNEL) live++;
    el.mLive.textContent = fmt(live);
    el.mBest.textContent = fmt(app.best);
    el.mEvents.textContent = fmt(app.eventCount);
    if (app.run && !app.finished) {
      el.clockPill.textContent = ((performance.now() - app.run.startedAt) / 1000).toFixed(2) + " s";
    }
  }

  function setStage(stage, value, mode) {
    const li = document.querySelector('.pipeline li[data-stage="' + stage + '"]');
    if (!li) return;
    if (value !== undefined && value !== null) li.querySelector(".val").textContent = value;
    if (mode) li.dataset.on = mode;
  }
  function resetPipeline() {
    document.querySelectorAll(".pipeline li").forEach((li) => {
      li.dataset.on = "";
      li.querySelector(".val").textContent = "—";
    });
  }

  function traceRow(kind, tag, ms, msg) {
    const row = document.createElement("div");
    row.className = "trace-item";
    row.dataset.kind = kind;
    const t = document.createElement("span"); t.className = "t"; t.textContent = (ms / 1000).toFixed(3) + "s";
    const g = document.createElement("span"); g.className = "tag"; g.textContent = tag;
    const m = document.createElement("span"); m.className = "msg"; m.textContent = msg;
    row.append(t, g, m);
    app.traceQueue.push(row);
  }

  function pushConsole(line) {
    app.consoleLines.push(line);
    if (app.consoleLines.length > MAX_CONSOLE_LINES) app.consoleLines.splice(0, app.consoleLines.length - MAX_CONSOLE_LINES);
    app.consoleDirty = true;
  }

  function setState(v, s) { if (v >= 1 && v <= app.graph.n) app.states[v] = s; }
  function setAll(list, s) { if (list) for (const v of list) setState(v, s); }

  // ── event handling ─────────────────────────────────────────────────────────
  function handle(ev) {
    app.eventCount++;
    const ms = ev.ms || 0;

    switch (ev.e) {
      case "meta":
        setStage("reduce", "running", "active");
        traceRow("info", "meta", ms, ev.msg);
        break;

      case "phase_start":
        traceRow("info", "phase", ms, ev.msg);
        if (/Nemhauser/.test(ev.msg || "")) { setStage("reduce", undefined, "done"); setStage("lp", "running", "active"); }
        break;

      case "phase":
        setStage("reduce", ev.live + " live", "active");
        traceRow("info", ev.phase, ms, ev.msg);
        break;

      case "rule": {
        const touched = [ev.v];
        if (ev.u !== undefined) touched.push(ev.u);
        if (ev.a !== undefined) touched.push(ev.a);
        if (ev.b !== undefined) touched.push(ev.b);
        if (ev.decide === "in") {
          setState(ev.v, S_IN);
          if (ev.u !== undefined && ev.rule === "R6") setState(ev.u, S_IN);
          setAll(ev.out, S_OUT);
          if (ev.out) for (const o of ev.out) touched.push(o);
        } else if (ev.decide === "out") {
          setState(ev.v, S_OUT);
        } else if (ev.decide === "fold") {
          setState(ev.v, S_FOLD);
          if (ev.rule === "R5" && ev.b !== undefined) setState(ev.b, S_FOLD);
        }
        GraphView.markTouched(touched);
        traceRow("rule", ev.rule, ms, ev.msg);
        break;
      }

      case "lp":
        setAll(ev.forced_in, S_IN);
        setAll(ev.forced_out, S_OUT);
        setAll(ev.half, S_KERNEL);
        GraphView.markTouched([].concat(ev.forced_in || [], ev.forced_out || []));
        setStage("lp", "flow " + fmt(ev.flow), "done");
        traceRow("lp", "LP/NT", ms, ev.msg);
        break;

      case "lp_evict":
        setAll(ev.out, S_OUT);
        break;

      case "kernel": {
        setAll(ev.vertices, S_KERNEL);
        const lpVal = document.querySelector('.pipeline li[data-stage="lp"] .val').textContent;
        setStage("lp", (lpVal === "running" || lpVal === "—") ? "skipped" : undefined, "done");
        setStage("kernel", ev.size + " vertices", "done");
        setStage("solve", ev.components + " component(s)", "active");
        el.mKernel.textContent = fmt(ev.size);
        el.mFolds.textContent = fmt(ev.folds);
        if (ev.components === 0) {
          el.components.innerHTML =
            '<p class="empty">Kernel is empty — the reduction rules decided every vertex, ' +
            'so no search was needed. The answer is exact.</p>';
          setStage("solve", "not needed", "done");
        }
        app.best = ev.base || 0;
        Chart.push(ms, app.best);
        traceRow("info", "kernel", ms, ev.msg);
        break;
      }

      case "component": {
        app.compVerts[ev.comp] = ev.vertices || [];
        if (ev.selected) {
          setAll(ev.vertices, S_OUT);
          setAll(ev.selected, S_IN);
        } else {
          setAll(ev.vertices, S_KERNEL);
        }
        addComponentRow(ev);
        traceRow("solve", ev.method === "tree-dp" ? "tree" : ev.method === "ils" ? "ILS" : "B&B", ms, ev.msg);
        break;
      }

      case "greedy":
      case "ils":
      case "ils_done": {
        const verts = app.compVerts[ev.comp] || [];
        if (ev.sel) {
          setAll(verts, S_KERNEL);
          setAll(ev.sel, S_IN);
        }
        updateComponentRow(ev.comp, ev.weight);
        if (ev.e !== "greedy" || true) traceRow("solve", ev.e === "greedy" ? "greedy" : ev.e === "ils" ? "ILS+" : "ILS✓", ms, ev.msg);
        break;
      }

      case "move": {
        setAll(ev.drop, S_KERNEL);
        setAll(ev.add, S_IN);
        GraphView.markTouched([].concat(ev.add || [], ev.drop || []));
        break;
      }

      case "best":
        app.best = ev.total;
        Chart.push(ms, ev.total);
        break;

      case "progress":
        GraphView.setOverlay("component " + ev.comp + "   iter " + fmt(ev.iter) +
                             "   best " + fmt(ev.best) + "   stall " + fmt(ev.stall));
        break;

      case "unfold":
        if (ev.type === "N-fold") {
          setState(ev.v, ev.anchor_in ? S_OUT : S_IN);
        } else {
          if (ev.anchor_in) { setState(ev.b, S_IN); setState(ev.v, S_OUT); }
          else { setState(ev.v, S_IN); setState(ev.b, S_OUT); }
        }
        GraphView.markTouched([ev.v, ev.a, ev.b].filter((x) => x !== undefined));
        app.unfolded = (app.unfolded || 0) + 1;
        setStage("solve", undefined, "done");
        setStage("unfold", app.unfolded + " fold(s)", "active");
        traceRow("info", "unfold", ms, ev.msg);
        break;

      case "verify":
        setStage("solve", undefined, "done");
        setStage("unfold", app.unfolded ? app.unfolded + " fold(s)" : "no folds", "done");
        setStage("verify", ev.ok ? "passed" : "FAILED", "done");
        traceRow("verify", "verify", ms, ev.msg);
        renderVerify(ev);
        break;

      case "done":
        for (let i = 1; i <= app.graph.n; i++) app.states[i] = S_OUT;
        setAll(ev.vertices, S_IN);
        GraphView.markTouched([]);
        app.best = ev.weight;
        Chart.push(ms, ev.weight);
        if (ev.optimal) Chart.setReference(ev.weight, "proven optimal");
        renderLedger(ev.rule_counts || {});
        renderDone(ev);
        traceRow("verify", "done", ms, ev.msg);
        GraphView.setOverlay("");
        break;

      case "stderr":
        traceRow("error", "stderr", ms, ev.msg);
        break;

      case "error":
        traceRow("error", "error", ms, ev.msg);
        break;

      case "end":
        finishRun(ev.returncode);
        break;
    }
  }

  // ── component panel ────────────────────────────────────────────────────────
  function addComponentRow(ev) {
    const empty = el.components.querySelector(".empty");
    if (empty) empty.remove();
    let row = document.getElementById("comp-" + ev.comp);
    if (!row) {
      row = document.createElement("div");
      row.className = "comp";
      row.id = "comp-" + ev.comp;
      row.innerHTML = '<span class="id"></span><span class="meth"></span><span class="w"></span>';
      el.components.appendChild(row);
    }
    row.dataset.method = ev.method;
    row.querySelector(".id").textContent = "#" + ev.comp;
    const label = ev.method === "tree-dp" ? "forest · exact DP"
      : ev.method === "branch-and-bound" ? "branch & bound · proven"
      : "ILS · heuristic";
    row.querySelector(".meth").textContent = ev.size + " vertices · " + label;
    row.querySelector(".w").textContent = ev.weight !== undefined ? fmt(ev.weight) : "…";
  }
  function updateComponentRow(comp, weight) {
    const row = document.getElementById("comp-" + comp);
    if (row) row.querySelector(".w").textContent = fmt(weight);
  }

  // ── correctness panel ──────────────────────────────────────────────────────
  function renderVerify(ev) {
    const ok = ev.ok;
    const box = document.createElement("div");
    box.className = "verdict";
    box.dataset.ok = String(!!ok);
    box.innerHTML =
      '<span class="icon">' + (ok ? "✓" : "✕") + "</span><div><b>" +
      (ok ? "Verified independent set" : "Verification failed") + "</b><br>" +
      "checked " + fmt(ev.edges_checked) + " edges · " + fmt(ev.conflicts) + " conflict(s) · " +
      fmt(ev.size) + " vertices · weight " + fmt(ev.weight) +
      (ev.accounting_ok ? " · solver bookkeeping agrees with a fresh recount" :
        " · <span class=\"flag\">bookkeeping disagrees (" + fmt(ev.reported) + ")</span>") +
      (ev.maximal ? "" : " · set is not maximal") +
      "</div>";
    const first = el.correctness.querySelector(".empty");
    if (first) first.remove();
    el.correctness.prepend(box);
  }

  function renderDone(ev) {
    const box = document.createElement("div");
    box.className = "verdict";
    box.dataset.ok = String(!!ev.verified);
    box.innerHTML =
      '<span class="icon">' + (ev.optimal ? "★" : "▸") + "</span><div><b>Weight " + fmt(ev.weight) +
      "</b> on " + fmt(ev.size) + " vertices" +
      (ev.optimal ? " — <b>proven optimal</b> (every component was solved exactly)"
                  : ev.stopped ? " — stopped early, best answer so far"
                  : " — heuristic result for the components too large to certify") +
      "<br>kernel " + fmt(ev.kernel_size) + " vertices · " + fmt(ev.folds) + " fold(s) reconstructed</div>";
    el.correctness.prepend(box);
  }

  function renderLedger(counts) {
    el.ledger.innerHTML = "";
    const keys = Object.keys(counts);
    if (!keys.length) {
      el.ledger.innerHTML = '<p class="empty">No reductions fired on this instance.</p>';
      return;
    }
    keys.sort();
    for (const k of keys) {
      const row = document.createElement("div");
      row.className = "row";
      row.innerHTML = '<span class="id"></span><span class="nm"></span><span class="ct"></span>';
      row.querySelector(".id").textContent = k;
      row.querySelector(".nm").textContent = RULE_NAMES[k] || "";
      row.querySelector(".ct").textContent = fmt(counts[k]);
      el.ledger.appendChild(row);
    }
  }

  // ── run lifecycle ──────────────────────────────────────────────────────────
  function collectOptions() {
    const disable = [];
    document.querySelectorAll('.rules input[data-rule]').forEach((cb) => {
      if (!cb.checked) disable.push(cb.dataset.rule);
    });
    return {
      seed: Number(el.engSeed.value) || 0,
      stall: Number(el.stall.value) || 0,
      exact_threshold: Number(el.exactThreshold.value) || 0,
      time_limit: Number(el.timeLimit.value) || 0,
      legacy_rules: el.legacyRules.checked,
      disable,
    };
  }

  async function startRun() {
    if (!app.graph) return;
    if (app.run && !app.finished) return;

    el.traceList.innerHTML = "";
    app.consoleLines = [];
    el.consoleOut.textContent = "";
    el.correctness.innerHTML = "";
    el.components.innerHTML = "";
    el.ledger.innerHTML = "";
    app.compVerts = {};
    app.eventCount = 0;
    app.best = 0;
    app.unfolded = 0;
    app.states = new Uint8Array(app.graph.n + 1);
    GraphView.setStates(app.states);
    GraphView.markTouched([]);
    GraphView.setOverlay("");
    Chart.reset();
    resetPipeline();

    let started;
    try {
      started = await api("/api/run", {
        graph: app.graph,
        options: collectOptions(),
      });
    } catch (err) {
      setPill(el.runPill, "error", "err");
      traceRow("error", "error", 0, String(err.message || err));
      markDirty();
      return;
    }

    el.cmdtext.textContent = started.cmd;
    app.finished = false;
    app.run = { id: started.run_id, startedAt: performance.now() };
    setPill(el.runPill, "running", "run");
    el.runBtn.disabled = true;
    el.stopBtn.disabled = false;

    const es = new EventSource("/api/events/" + started.run_id);
    app.run.es = es;
    es.onmessage = (msg) => {
      let ev;
      try { ev = JSON.parse(msg.data); } catch (_) { return; }
      pushConsole(msg.data);
      handle(ev);
      markDirty();
      if (ev.e === "end") es.close();
    };
    es.onerror = () => {
      if (!app.finished) { setPill(el.runPill, "stream lost", "err"); finishRun(null); }
      es.close();
    };

    tick();
  }

  function tick() {
    if (app.finished) return;
    updateMetrics();
    requestAnimationFrame(tick);
  }

  function finishRun(rc) {
    app.finished = true;
    el.runBtn.disabled = false;
    el.stopBtn.disabled = true;
    if (rc === 0 || rc === null || rc === undefined) setPill(el.runPill, "finished", "ok");
    else setPill(el.runPill, "exit " + rc, "err");
    markDirty();
  }

  async function stopRun() {
    if (!app.run || app.finished) return;
    el.stopBtn.disabled = true;
    setPill(el.runPill, "stopping…", "run");
    try { await api("/api/stop/" + app.run.id, {}); } catch (_) { /* the stream reports the outcome */ }
  }

  // ── instance handling ──────────────────────────────────────────────────────
  function applyGraph(g) {
    app.graph = g;
    app.states = new Uint8Array(g.n + 1);
    GraphView.setGraph(g);
    GraphView.setStates(app.states);
    el.graphMeta.textContent = g.n + " vertices · " + g.edges.length + " edges · " +
      (g.kind || "custom") + (g.seed ? " · seed " + g.seed : "");
    el.mN.textContent = fmt(g.n);
    el.mM.textContent = fmt(g.edges.length);
    el.mKernel.textContent = "—";
    el.mFolds.textContent = "0";
    app.best = 0;
    Chart.reset();
    resetPipeline();
    el.correctness.innerHTML = '<p class="empty">Run the engine, then use <b>Compare solvers</b> or <b>Certify optimum</b>.</p>';
    el.components.innerHTML = '<p class="empty">Nothing yet.</p>';
    markDirty();
  }

  async function generate() {
    try {
      const data = await api("/api/generate", {
        kind: el.kind.value,
        n: Number(el.nodes.value),
        seed: Number(el.seed.value),
        density: Number(el.density.value),
        wmax: Number(el.wmax.value),
      });
      applyGraph(data.graph);
    } catch (err) {
      setPill(el.runPill, "generate failed", "err");
      traceRow("error", "error", 0, String(err.message || err));
      markDirty();
    }
  }

  async function compareSolvers() {
    if (!app.graph) return;
    el.compareBtn.disabled = true;
    const original = el.compareBtn.textContent;
    el.compareBtn.textContent = "comparing…";
    switchTab("correct");
    try {
      const r = await api("/api/compare", { graph: app.graph });
      const rows = [
        ["engine (corrected rules)", r.engine],
        ["engine with solution.cpp's rule variants", r.legacy_rules],
        ["solution.cpp binary", r.solution_cpp],
      ];
      const best = Math.max(...rows.map(([, v]) => (v && v.valid ? v.weight : -1)));
      const table = document.createElement("table");
      table.className = "cmp";
      let html = "<thead><tr><th>solver</th><th>weight</th><th>size</th><th>valid</th><th></th></tr></thead><tbody>";
      for (const [name, v] of rows) {
        if (!v) continue;
        const gap = r.optimum !== undefined ? r.optimum - v.weight : null;
        html += '<tr data-best="' + (v.valid && v.weight === best) + '">' +
          "<td>" + name + "</td>" +
          '<td class="num">' + fmt(v.weight) + "</td>" +
          '<td class="num">' + fmt(v.size) + "</td>" +
          "<td>" + (v.valid ? "yes" : '<span class="flag">NO — ' + v.conflicts + " conflicts</span>") + "</td>" +
          "<td>" + (gap === null ? "" : gap === 0 ? "optimal" :
            '<span class="flag">' + gap + " below optimum</span>") + "</td></tr>";
      }
      html += "</tbody>";
      table.innerHTML = html;
      const head = document.createElement("div");
      head.className = "verdict";
      head.dataset.ok = String(r.optimum === undefined || (r.engine && r.engine.weight === r.optimum));
      head.innerHTML = '<span class="icon">=</span><div><b>Solver comparison</b><br>' +
        (r.optimum !== undefined
          ? "proven optimum for this instance is <b>" + fmt(r.optimum) + "</b> (independent exact search)"
          : "instance is larger than 64 vertices, so no exact certificate was computed") +
        "</div>";
      el.correctness.prepend(table);
      el.correctness.prepend(head);
    } catch (err) {
      traceRow("error", "compare", 0, String(err.message || err));
      markDirty();
    } finally {
      el.compareBtn.disabled = false;
      el.compareBtn.textContent = original;
    }
  }

  async function certify() {
    if (!app.graph) return;
    el.exactBtn.disabled = true;
    const original = el.exactBtn.textContent;
    el.exactBtn.textContent = "certifying…";
    switchTab("correct");
    try {
      const r = await api("/api/exact", { graph: app.graph });
      Chart.setReference(r.weight, "optimum " + r.weight);
      const box = document.createElement("div");
      box.className = "verdict";
      const matched = app.best === r.weight;
      box.dataset.ok = String(matched || app.best === 0);
      box.innerHTML = '<span class="icon">★</span><div><b>Proven optimum: ' + fmt(r.weight) + "</b><br>" +
        "independent exact search explored " + fmt(r.nodes) + " nodes" +
        (app.best ? " · engine found " + fmt(app.best) +
          (matched ? " — <b>match</b>" : ' — <span class="flag">' + (r.weight - app.best) + " short</span>") : "") +
        "</div>";
      el.correctness.prepend(box);
    } catch (err) {
      const box = document.createElement("div");
      box.className = "verdict";
      box.dataset.ok = "false";
      box.innerHTML = '<span class="icon">!</span><div>' + String(err.message || err) + "</div>";
      const first = el.correctness.querySelector(".empty");
      if (first) first.remove();
      el.correctness.prepend(box);
    } finally {
      el.exactBtn.disabled = false;
      el.exactBtn.textContent = original;
    }
  }

  function switchTab(name) {
    document.querySelectorAll(".tab").forEach((t) => t.classList.toggle("active", t.dataset.tab === name));
    document.querySelectorAll(".tab-body").forEach((b) => { b.hidden = b.id !== "tab-" + name; });
  }

  // ── boot ───────────────────────────────────────────────────────────────────
  async function boot() {
    GraphView.init($("graphCanvas"), $("tooltip"), $("canvasOverlay"), $("canvasEmpty"));
    Chart.init($("chartCanvas"));

    el.nodes.addEventListener("input", () => { el.nodesOut.textContent = el.nodes.value; });
    el.density.addEventListener("input", () => { el.densityOut.textContent = Number(el.density.value).toFixed(2); });
    el.genBtn.addEventListener("click", generate);
    el.runBtn.addEventListener("click", startRun);
    el.stopBtn.addEventListener("click", stopRun);
    el.compareBtn.addEventListener("click", compareSolvers);
    el.exactBtn.addEventListener("click", certify);
    el.fitBtn.addEventListener("click", () => GraphView.fit());
    el.labelsBtn.addEventListener("click", () => {
      GraphView.labels = !GraphView.labels;
      el.labelsBtn.setAttribute("aria-pressed", String(GraphView.labels));
      GraphView.draw();
    });
    const themeBtn = $("themeBtn");
    try {
      const saved = localStorage.getItem("mwis-theme");
      if (saved) document.documentElement.dataset.theme = saved;
    } catch (_) { /* private mode: keep the default */ }
    themeBtn.addEventListener("click", () => {
      const next = document.documentElement.dataset.theme === "light" ? "dark" : "light";
      document.documentElement.dataset.theme = next;
      try { localStorage.setItem("mwis-theme", next); } catch (_) { /* ignore */ }
      GraphView.draw();
      Chart.draw();
    });
    el.clearLog.addEventListener("click", () => {
      el.traceList.innerHTML = "";
      app.consoleLines = [];
      el.consoleOut.textContent = "";
    });
    document.querySelectorAll(".tab").forEach((t) => t.addEventListener("click", () => switchTab(t.dataset.tab)));
    el.pasteBtn.addEventListener("click", () => el.pasteDialog.showModal());
    el.pasteDialog.addEventListener("close", () => {
      if (el.pasteDialog.returnValue !== "load") return;
      try {
        applyGraph(parseEngineText(el.pasteText.value));
      } catch (err) {
        setPill(el.runPill, "bad instance", "err");
        traceRow("error", "parse", 0, String(err.message || err));
        markDirty();
      }
    });
    window.addEventListener("keydown", (e) => {
      if (e.key === "Enter" && (e.metaKey || e.ctrlKey)) { e.preventDefault(); startRun(); }
      if (e.key === "Escape" && !app.finished) stopRun();
    });

    try {
      const health = await api("/api/health");
      for (const k of health.kinds) {
        const opt = document.createElement("option");
        opt.value = k;
        opt.textContent = k;
        el.kind.appendChild(opt);
      }
      el.kind.value = "random";
      setPill(el.buildPill, health.ok ? "engine: built" : "engine: build failed", health.ok ? "ok" : "err");
      if (!health.ok) pushConsole(health.build || "");
    } catch (err) {
      setPill(el.buildPill, "backend unreachable", "err");
      return;
    }

    await generate();
    setPill(el.runPill, "ready", "idle");
  }

  boot();
})();
