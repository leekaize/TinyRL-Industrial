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

function label(node, slug, action) { post("/label", {node, slug, action}); }
function inject(ev) { post("/machine", {ev}); }

const PREFILL = {unconfirmed: "op_change", corroborated: "fault_heavy",
                 known: "", unconsulted: ""};
const WHY = {
  known: "a sibling library already knows this condition",
  corroborated: "a sibling node is ALARMING at the same time - likely a real fault",
  unconfirmed: "siblings report normal. This may be an operating-point change " +
               "(name it so the node learns it) or a fault the sibling cannot see",
  unconsulted: "no sibling evidence available (single node or not ready)",
};

function askHTML(a) {
  const v = a.verdict || {verdict: "unconsulted", text: ""};
  return `<b>Node ${a.node}</b> opened an UNKNOWN condition at step ${a.step}.` +
    `<br>Collective verdict: <b>${v.verdict}</b> - ${WHY[v.verdict] || ""}` +
    (v.text ? `<br><span class="ev-line">consulted: ${v.text}</span>` : "") +
    `<br><button onclick="label(${a.node},document.getElementById('c${a.node}').value,'label')">label as condition</button>` +
    `<input id="c${a.node}" value="${PREFILL[v.verdict] || ""}" size="12">` +
    `<button class="warn" onclick="label(${a.node},'','normal')">false alarm (normal)</button>` +
    `<button class="warn" onclick="label(${a.node},'','fault')">confirm fault</button>` +
    `<button class="grey" onclick="post('/control',{cmd:'continue'})">continue without answering</button>`;
}

async function poll() {
  const st = await (await fetch("/state")).json();
  $("hdr").textContent = `${st.nodes.length} node(s) | ${st.steps_total} steps`;
  $("status").textContent =
    st.done ? "DONE" : st.holding ? "HOLDING - answer the ask" :
    st.playing ? "running" : "paused";
  $("b_play").textContent = st.playing && !st.holding ? "pause" : "play";

  const m = $("m_state");
  m.textContent = st.machine;
  m.className = st.machine === "normal" ? "m-normal" : "m-fault";

  const ask = $("ask");
  if (st.ask.length && st.holding) {
    ask.classList.remove("hidden");
    ask.innerHTML = st.ask.map(askHTML).join("<hr>");
  } else { ask.classList.add("hidden"); }

  $("cards").innerHTML = st.nodes.map(n =>
    `<div class="card"><b>node ${n.id}</b> (${n.node_type}) - step ${n.step}<br>` +
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
    if (!S.charts.length) buildCharts(st);
    st.nodes.forEach((n, i) => updateCharts(i, n));
  }
  $("events").textContent = st.events.map(e => `s${e[0]}  ${e[1]}`).join("\n");
}

function buildCharts(st) {
  st.nodes.forEach((n, i) => {
    const h = document.createElement("h2");
    h.textContent = `node ${n.id} (${n.node_type})`;
    $("charts").appendChild(h);
    const ds = document.createElement("div");
    ds.innerHTML = `<canvas id="sc${i}" height="120"></canvas>` +
                   `<canvas id="fc${i}" height="140"></canvas>`;
    $("charts").appendChild(ds);
    S.charts.push({
      score: new Chart($("sc" + i), {type: "line",
        data: {labels: [], datasets: [
          {label: "score", data: [], borderColor: "#2563eb", pointRadius: 0},
          {label: "theta", data: [], borderColor: "#dc2626",
           borderDash: [6, 4], pointRadius: 0}]},
        options: {animation: false, scales: {y: {type: "logarithmic"}}}}),
      feat: new Chart($("fc" + i), {type: "line",
        data: {labels: [], datasets: n.feature_names.map((nm, k) =>
          ({label: nm, data: [], borderColor: COLORS[k % COLORS.length],
            pointRadius: 0, borderWidth: 1}))},
        options: {animation: false, scales: {y: {min: 0, max: 1}}}}),
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

$("b_play").onclick = () => post("/control", {cmd: "play"});
$("b_step").onclick = () => post("/control", {cmd: "step"});
$("speed").onchange = e => post("/control", {cmd: "speed", speed: +e.target.value});
$("b_export").onclick = async () => {
  const r = await (await post("/export", {})).json();
  $("status").textContent = "exported: " + r.file;
};

poll();
setInterval(poll, 500);
