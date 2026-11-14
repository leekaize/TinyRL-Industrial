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

function label(node, slug, action, kind) {
  post("/label", {node, slug, action, kind: kind || "fault"});
}
function inject(ev) { post("/machine", {ev}); }

const PREFILL = {unconfirmed: "imbalance", corroborated: "phase_loss",
                 known: "imbalance", unconsulted: "imbalance"};
const FAULT_STATES = ["imbalance", "loose_mount", "phase_loss"];
const WHY = {
  known: "a sibling library already knows this condition",
  corroborated: "a sibling node is ALARMING at the same time - likely a real fault",
  unconfirmed: "siblings report normal. This may be an operating-point change " +
               "(name it so the node learns it) or a fault the sibling cannot see",
  unconsulted: "no sibling evidence available (single node or not ready)",
};

// faults are labelled WITH their operating point (compound slug:
// imbalance@op_speed_1) so the same fault at different speeds is a
// different library condition; normal-condition asks prefill with the
// machine truth itself
function prefillFor(st) {
  if (FAULT_STATES.includes(st.machine)) {
    const v = (st.ask[0].verdict || {}).verdict || "unconfirmed";
    return `${PREFILL[v] || "imbalance"}@${st.context}`;
  }
  return st.machine;
}

// ONE popup for every pending ask: one label resolves all of them (the
// collective shares it - sibling episodes get the slug applied to their
// own episode means)
function askPanelHTML(st) {
  const lines = st.ask.map(a => {
    const v = a.verdict || {verdict: "unconsulted", text: ""};
    return `<b>node ${a.node}</b> step ${a.step}: <b>${v.verdict}</b>` +
      (v.text ? ` <span class="ev-line">consulted: ${v.text}</span>` : "");
  }).join(" &nbsp;|&nbsp; ");
  const n0 = st.ask[0].node;
  const why = WHY[(st.ask[0].verdict || {}).verdict] || "";
  return `<b>${st.ask.length} node(s) opened an UNKNOWN condition</b> - ${lines}` +
    `<br>${why}` +
    `<br>one label resolves every pending ask: ` +
    `<input id="askslug" value="${prefillFor(st)}" size="26"> ` +
    `<button onclick="label(${n0},document.getElementById('askslug').value,'label','normal')">label as normal</button>` +
    `<button onclick="label(${n0},document.getElementById('askslug').value,'label','fault')">label as fault</button>` +
    `<button class="warn" onclick="label(${n0},'','normal')">false alarm (no name)</button>` +
    `<button class="grey" onclick="post('/control',{cmd:'continue'})">continue without answering</button>`;
}

async function poll() {
  const st = await (await fetch("/state")).json();
  $("hdr").textContent = `${st.nodes.length} node(s) | ${st.steps_total} steps`;
  $("status").textContent =
    st.done ? "DONE" : st.holding ? "HOLDING - answer the ask" :
    st.playing ? "running" : "idle - use +5 s / +60 s";

  const m = $("m_state");
  m.textContent = st.machine;
  m.className = ["imbalance", "loose_mount", "phase_loss"].includes(st.machine)
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

function buildCharts(st) {
  S.marks = st.marks || [];
  st.nodes.forEach((n, i) => {
    const h = document.createElement("h2");
    h.textContent = `node ${n.id} (${n.node_type})`;
    $("charts").appendChild(h);
    const ds = document.createElement("div");
    ds.innerHTML = `<canvas id="sc${i}" height="120"></canvas>` +
                   `<div class="chartnote">score = prediction surprise (EMA of ` +
                   `one-step error, log scale). theta = alarm threshold, fit at ` +
                   `the calibration operating point - other operating points ` +
                   `exceed it until labelled once, then sit quiet as recognised ` +
                   `conditions. grey dashed = machine events.</div>` +
                   `<canvas id="fc${i}" height="140"></canvas>` +
                   `<div class="chartnote">features are min-max normalised per view ` +
                   `so mixed units share one panel.</div>`;
    $("charts").appendChild(ds);
    S.charts.push({
      score: new Chart($("sc" + i), {type: "line",
        data: {labels: [], datasets: [
          {label: "score", data: [], borderColor: "#2563eb", pointRadius: 0},
          {label: "theta", data: [], borderColor: "#dc2626",
           borderDash: [6, 4], pointRadius: 0}]},
        options: {animation: false, scales: {y: {type: "logarithmic"}},
                  plugins: [machineMarks]}}),
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

$("b_run5").onclick = () => post("/control", {cmd: "run", seconds: 5});
$("b_run60").onclick = () => post("/control", {cmd: "run", seconds: 60});
$("b_export").onclick = async () => {
  const r = await (await post("/export", {})).json();
  $("status").textContent = "exported: " + r.file;
};

poll();
setInterval(poll, 500);
