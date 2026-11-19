// Interactive bench frontend. Defensive on purpose: any runtime error is
// shown in the red bar at the top instead of silently blanking the page.
"use strict";

const COLORS = ["#2563eb", "#0e9f6e", "#d97706", "#dc2626", "#7c3aed", "#0e7490"];
const S = { charts: [], warned: false };
const $ = id => document.getElementById(id);

function fail(msg) {
  const bar = $("errbar");
  bar.classList.remove("hidden");
  bar.textContent = msg;
}
window.onerror = (msg, src, line) => fail(`JS error (${src}:${line}): ${msg}`);

async function post(path, body) {
  const r = await fetch(path, {method: "POST", body: JSON.stringify(body || {})});
  if (!r.ok) fail(`${path} failed: HTTP ${r.status}`);
  return r;
}

function label(node, slug, action, kind, batch) {
  post("/label", {node, slug, action, kind: kind || "fault",
                  batch: batch || ""});
}
function discard(node, batch) { post("/discard", {node, batch}); }
function inject(ev) { post("/machine", {ev}); }

const FAULT_STATES = ["imbalance", "phase_loss"];

// condition vocabulary (2026-11-15): normals are op_stopped / speed_1 /
// speed_2, faults compound with the running setpoint (speed_1_imbalance)
function truthSlug(st) {
  if (FAULT_STATES.includes(st.machine))
    return st.context.replace(/^op_/, "") + "_" + st.machine;
  return st.machine === "op_stopped" ? "op_stopped"
                                     : st.machine.replace(/^op_/, "");
}

// ONE popup for every pending ask: one label resolves all of them. The
// bench applies the same slug to every node of the machine with an open
// ask (the same-timeframe event); each node relabels its own episode and
// consumes its own stored batch. The label lands on the asked node's
// STORED batch when one exists (settled data, works late); otherwise the
// live episode.
function askPanelHTML(st) {
  const lines = st.ask.map(a =>
    `<b>node ${a.node}</b> step ${a.step}`).join(" &nbsp;|&nbsp; ");
  const n0 = st.ask[0].node;
  const pb = (st.pending[n0] || [])[0];
  const bid = pb ? pb.id : "";
  const via = bid
    ? `batch <b>${bid}</b> (n=${pb.n} settled samples)`
    : `the live episode (no stored batch yet)`;
  return `<b>${st.ask.length} node(s) opened an UNKNOWN condition</b> - ${lines}` +
    `<br>one label resolves every pending ask of the machine via ${via}: ` +
    `<input id="askslug" value="${truthSlug(st)}" size="26"> ` +
    `<button onclick="label(${n0},document.getElementById('askslug').value,'label','normal','${bid}')">label as normal</button>` +
    `<button onclick="label(${n0},document.getElementById('askslug').value,'label','fault','${bid}')">label as fault</button>` +
    `<button class="warn" onclick="label(${n0},'','normal')">false alarm (no name)</button>` +
    `<button class="grey" onclick="post('/control',{cmd:'continue'})">continue without answering</button>`;
}

// pending batches on a node card: label or discard BY ID (late labels
// are the point of the pending store)
function pendHTML(st, node) {
  const pb = st.pending[node] || [];
  if (!pb.length) return "";
  return "<br>pending: " + pb.map(b =>
    `${b.id} (n=${b.n}) ` +
    `<button class="grey" onclick="label(${node},prompt('label for ${b.id}'), 'label','fault','${b.id}')">label</button>` +
    `<button class="warn" onclick="discard(${node},'${b.id}')">discard</button>`
  ).join(" ");
}

async function poll() {
  const st = await (await fetch("/state")).json();
  $("hdr").textContent = `${st.nodes.length} node(s) | ${st.steps_total} steps`;
  $("status").textContent =
    st.done ? "DONE" : st.holding ? "HOLDING - answer the ask" :
    st.playing ? "running" : "idle - use +5 s / +60 s";

  const m = $("m_state");
  m.textContent = st.machine;
  m.className = ["imbalance", "phase_loss"].includes(st.machine)
    ? "m-fault" : "m-normal";

  const ask = $("ask");
  if (st.ask.length && st.holding) {
    ask.classList.remove("hidden");
    // rebuilding innerHTML wipes the focused input and resets it to the
    // prefill - skip the refresh while the operator is typing (found
    // 2026-11-14: typing snapped back to the prefill every 500 ms poll)
    const el = document.activeElement;
    if (!(el && ask.contains(el) && el.tagName === "INPUT"))
      ask.innerHTML = askPanelHTML(st);
  } else { ask.classList.add("hidden"); }

  $("cards").innerHTML = st.nodes.map(n =>
    `<div class="card"><b>node ${n.id}</b> (${n.node_type}) - step ${n.step} - machine: ${st.machine}<br>` +
    `phase=${n.phase} cond=${n.cond} lib=${n.lib}<br>` +
    n.feat.map((v, k) =>
      `${n.feature_names[k]}=${v.toPrecision(4)}${n.feature_units[k]}`)
      .join(" | ") +
    pendHTML(st, n.id) +
    `</div>`).join("");

  if (typeof Chart === "undefined") {
    if (!S.warned) {
      S.warned = true;
      $("charts").innerHTML =
        "<p>Chart.js failed to load (offline?) - node cards, asks and " +
        "events still work.</p>";
    }
  } else {
    S.marks = st.marks || [];
    S.libs = st.libs || S.libs || [];
    if (!S.charts.length) buildCharts(st);
    st.nodes.forEach((n, i) => updateCharts(i, n));
  }
  $("events").textContent = st.events.map(e => `s${e[0]}  ${e[1]}`).join("\n");
}

const machineMarks = {
  id: "machineMarks",
  afterDraw(c) {
    const marks = S.marks || [];
    if (!marks.length || !c.chartArea) return;
    const labels = c.data.labels || [];
    const ctx = c.ctx, x = c.scales.x, area = c.chartArea;
    ctx.save();
    ctx.strokeStyle = "#94a3b8";
    ctx.setLineDash([3, 3]);
    marks.forEach(m => {
      const idx = labels.indexOf(m[0]);
      if (idx < 0) return;
      const px = x.getPixelForValue(idx);
      if (px < area.left || px > area.right) return;
      ctx.beginPath();
      ctx.moveTo(px, area.top);
      ctx.lineTo(px, area.bottom);
      ctx.stroke();
    });
    ctx.restore();
  },
};

// span shading: teal = KNOWN (a named condition - quiet is positional,
// the score there cannot alarm); red = alarm phase. Set c.$spans before
// update(); each span is {a, b, color, alpha} in label-index terms.
const spanShades = {
  id: "spanShades",
  beforeDatasetsDraw(c) {
    const spans = c.$spans || [];
    if (!spans.length || !c.chartArea) return;
    const labels = c.data.labels || [];
    const x = c.scales.x, area = c.chartArea, ctx = c.ctx;
    const px = step => {
      const idx = labels.indexOf(step);
      return idx < 0 ? null : x.getPixelForValue(idx);
    };
    ctx.save();
    spans.forEach(s => {
      const a = px(s.a), b = px(s.b);
      if (a === null || b === null) return;
      ctx.fillStyle = s.color;
      ctx.globalAlpha = s.alpha;
      ctx.fillRect(Math.max(a, area.left), area.top,
                   Math.min(b, area.right) - Math.max(a, area.left),
                   area.bottom - area.top);
    });
    ctx.globalAlpha = 1;
    ctx.restore();
  },
};

// contiguous runs of a predicate over the per-step series -> {a, b} spans
function runsWhere(series, pred) {
  const out = [];
  let start = null;
  series.forEach((v, k) => {
    if (pred(v)) {
      if (start === null) start = k;
    } else if (start !== null) {
      out.push([start, k - 1]);
      start = null;
    }
  });
  if (start !== null) out.push([start, series.length - 1]);
  return out;
}

const KNOWN_EXCLUDED = new Set(["", "idle", "learning", "normal",
                                "unknown", "unidentified"]);

function scoreSpans(n) {
  const spans = [];
  runsWhere(n.series.cond, cd => !KNOWN_EXCLUDED.has(cd))
    .forEach(([a, b]) => spans.push(
      {a: n.series.steps[a], b: n.series.steps[b],
       color: "#0e9f6e", alpha: 0.10}));
  runsWhere(n.series.phase, p => p === "alarm")
    .forEach(([a, b]) => spans.push(
      {a: n.series.steps[a], b: n.series.steps[b],
       color: "#dc2626", alpha: 0.15}));
  return spans;
}

function buildCharts(st) {
  S.marks = st.marks || [];
  st.nodes.forEach((n, i) => {
    const h = document.createElement("h2");
    h.textContent = `node ${n.id} (${n.node_type})`;
    $("charts").appendChild(h);
    const ds = document.createElement("div");
    ds.innerHTML = `<canvas id="sc${i}" height="120"></canvas>` +
                   `<div class="chartnote">score = prediction surprise (EMA of ` +
                   `one-step error, log scale). RED dashed = the fit theta, ` +
                   `fit once at commissioning - it gates only UNKNOWN states. ` +
                   `DARK dash-dot = the threshold that actually applies: a ` +
                   `named condition is judged against its own learned band. ` +
                   `TEAL shading = known state (quiet is positional there - ` +
                   `the score cannot alarm); RED shading = alarm. ` +
                   `grey dashed = machine events.</div>` +
                   `<canvas id="fc${i}" height="140"></canvas>` +
                   `<div class="chartnote">features are min-max normalised per view ` +
                   `so mixed units share one panel.</div>`;
    $("charts").appendChild(ds);
    S.charts.push({
      score: new Chart($("sc" + i), {type: "line",
        data: {labels: [], datasets: [
          {label: "score", data: [], borderColor: "#2563eb", pointRadius: 0},
          {label: "fit theta (gates unknown)", data: [],
           borderColor: "#dc2626", borderDash: [6, 4], pointRadius: 0},
          {label: "own band (matched condition)", data: [],
           borderColor: "#7c2d12", borderDash: [2, 2], pointRadius: 0,
           borderWidth: 1.5}]},
        options: {animation: false, scales: {y: {type: "logarithmic"}},
                  plugins: [spanShades, machineMarks]}}),
      feat: new Chart($("fc" + i), {type: "line",
        data: {labels: [], datasets: n.feature_names.map((nm, k) =>
          ({label: nm, data: [], borderColor: COLORS[k % COLORS.length],
            pointRadius: 0, borderWidth: 1}))},
        options: {animation: false, scales: {y: {min: 0, max: 1}},
                  plugins: [machineMarks]}}),
    });
  });
}

function updateCharts(i, n) {
  const c = S.charts[i];
  c.score.data.labels = n.series.steps;
  c.score.data.datasets[0].data = n.series.score;
  c.score.data.datasets[1].data = n.series.theta.map(v => v > 0 ? v : null);
  // the threshold that actually applies: a named condition's own band
  // (from the LIBX rows), falling back to the global fit theta
  const lib = {};
  (S.libs[i] || []).forEach(e => { lib[e.slug] = e.theta; });
  c.score.data.datasets[2].data = n.series.cond.map((cd, k) => {
    const t = lib[cd];
    return (t && t > 0) ? t : (n.series.theta[k] > 0 ? n.series.theta[k] : null);
  });
  c.score.$spans = scoreSpans(n);
  c.score.update("none");
  // per-feature min-max normalisation so mixed units share one panel
  c.feat.data.labels = n.series.steps;
  n.series.feat.forEach((vals, k) => {
    const lo = Math.min(...vals), hi = Math.max(...vals);
    const rng = hi - lo || 1;
    c.feat.data.datasets[k].data = vals.map(v => (v - lo) / rng);
  });
  c.feat.update("none");
}

$("b_learn").onclick = () => post("/control", {cmd: "learn"});
$("b_run5").onclick = () => post("/control", {cmd: "run", seconds: 5});
$("b_run60").onclick = () => post("/control", {cmd: "run", seconds: 60});
$("b_export").onclick = async () => {
  const r = await (await post("/export", {})).json();
  $("status").textContent = "exported: " + r.file;
};

poll();
setInterval(poll, 500);
