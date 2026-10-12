// MusicalBOX SM - web editor. Plain JavaScript, no libraries: served by
// control/mbx_control.py from the Pi itself, works without internet.
"use strict";

// ---------------------------------------------------------------------------
// Texts
// ---------------------------------------------------------------------------
const FX_NAMES = {
  tone: "Equalizzatore", echo: "Eco a nastro", chorus: "Chorus", phaser: "Phaser", leslie: "Leslie",
  tremolo: "Tremolo", fuzz: "Fuzz", width: "Width", room: "Stanza", motor: "Motore (organo)",
};
const LABELS = {
  low_cut_hz: "Taglio bassi", bass_db: "Bassi", bass_hz: "Frequenza bassi", mid_db: "Medi", mid_hz: "Frequenza medi",
  treble_db: "Acuti", treble_hz: "Frequenza acuti", high_cut_hz: "Taglio acuti", gain_db: "Volume", time_ms: "Tempo",
  feedback: "Ripetizioni", mix: "Mix", tone_hz: "Tono", wow: "Wow", drive: "Drive", ping_pong: "Ping-pong",
  mode: "Tipo", rate_hz: "Velocità", depth: "Profondità", stages: "Stadi", stereo: "Stereo", speed: "Velocità",
  spread: "Apertura microfoni", cabinet: "Cabinet (riflessioni)", top_hz: "Limite acuti", shape: "Forma",
  octave: "Ottava", bias: "Bias", blend: "Fuzz / pulito", level_db: "Livello", amount: "Quantità", level: "Livello",
  time_s: "Durata", damp_hz: "Smorzamento", predelay_ms: "Pre-delay", attack_ms: "Attacco", decay_ms: "Decadimento",
  sustain: "Sostegno", release_ms: "Rilascio", levels: "Livelli", curve: "Curva", bypass: "Escluso",
  state: "Motore acceso", stop_s: "Tempo di arresto", start_s: "Tempo di ripartenza",
};
const ENUM_LABELS = {
  slow: "Lento", fast: "Veloce", ce2: "Chorus", ensemble: "Ensemble", sine: "Sinusoide", triangle: "Triangolo",
  square: "Quadra", linear: "Lineare", exp: "Esponenziale", on: "Sì", off: "No", A: "A", B: "B", AB: "A+B",
};
const NOTE_NAMES = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"];
const noteName = (n) => NOTE_NAMES[n % 12] + (Math.floor(n / 12) - 2);  // C3 = 60, as the engine
function noteNumber(v) {
  if (typeof v === "number") return v;
  const m = /^([A-Ga-g])(#|b)?(-?\d{1,2})$/.exec(String(v));
  if (!m) return 60;
  const base = { c: 0, d: 2, e: 4, f: 5, g: 7, a: 9, b: 11 }[m[1].toLowerCase()];
  return (parseInt(m[3], 10) + 2) * 12 + base + (m[2] === "#" ? 1 : m[2] === "b" ? -1 : 0);
}

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
const S = {
  tab: "panel",
  panelMode: "play",      // play | assign
  editing: null,          // program id in the editor
  saved: null,            // {programs, presets} as in the files
  draft: null,            // the same, being edited
  schema: {},
  instrumentsDef: [],     // instruments.json
  loaded: [],             // instruments the engine has in RAM
  bank: null,             // what the engine plays (programs available)
  status: {},
  backups: [],
  warnings: [],
  midiSeen: 0,
  kbBase: 48,
  hadComments: [],
};
const $ = (sel) => document.querySelector(sel);
const clone = (o) => JSON.parse(JSON.stringify(o));
const esc = (s) => String(s ?? "").replace(/[&<>"']/g, (c) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" }[c]));

async function api(path, body) {
  const opt = body === undefined ? {} : { method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify(body) };
  try {
    const r = await fetch(path, opt);
    return await r.json();
  } catch (e) {
    return { ok: false, error: "Pi non raggiungibile", offline: true };
  }
}
const cmd = (c) => api("/api/cmd", c);

function toast(text, error = false, ms = 3500) {
  const t = $("#toast");
  t.innerHTML = text;
  t.className = "toast" + (error ? " error" : "");
  t.hidden = false;
  clearTimeout(toast.timer);
  toast.timer = setTimeout(() => (t.hidden = true), ms);
}

// ---------------------------------------------------------------------------
// Loading and saving
// ---------------------------------------------------------------------------
const PRESET_MAX = 128;  // MIDI Program Change 0..127
const ROW = 8;           // pads per row on the panel page

// Only the defined presets, in numeric order (the file may list them in any order).
function sortPresets(table) {
  const out = {};
  Object.keys(table || {}).map(Number).filter((n) => n >= 1 && n <= PRESET_MAX && table[String(n)])
    .sort((a, b) => a - b).forEach((n) => { out[String(n)] = table[String(n)]; });
  return out;
}
function normalisePresets(p) {
  const out = Object.assign({}, p || {});  // keeps the order of the other keys in the file
  if (out.version === undefined) out.version = 1;
  if (out.startup === undefined) out.startup = 1;
  out.presets = sortPresets(out.presets);
  return out;
}
function setPreset(slot, id) {
  const pr = S.draft.presets;
  const table = Object.assign({}, pr.presets);
  if (id) table[String(slot)] = id; else delete table[String(slot)];
  pr.presets = sortPresets(table);
}

async function loadState() {
  const st = await api("/api/state");
  if (st.offline) { $("#offlineBar").hidden = false; setTimeout(loadState, 2000); return; }
  S.schema = st.schema || {};
  S.instrumentsDef = (st.instruments && st.instruments.instruments) || [];
  S.loaded = (st.instruments_loaded && st.instruments_loaded.instruments) || [];
  S.bank = st.bank && st.bank.ok ? st.bank : null;
  S.backups = st.backups || [];
  S.hadComments = st.had_comments || [];
  S.saved = { programs: st.programs || { version: 1, programs: [] }, presets: normalisePresets(st.presets) };
  if (!S.draft) S.draft = clone(S.saved);
  if (st.errors && st.errors.length) toast(st.errors.map(esc).join("<br>"), true, 8000);
  render();
}

const isDirty = () => S.draft && JSON.stringify(S.draft) !== JSON.stringify(S.saved);
function updateDirty() { $("#dirtyBar").hidden = !isDirty(); }

// Every change: the engine plays the draft right away (not saved).
function changed(structural = false) {
  updateDirty();
  schedulePreview();
  if (structural) render();
}

function previewPresets() {
  const presets = clone(S.draft.presets);
  if (S.editing) {
    const assigned = Object.values(presets.presets).includes(S.editing);
    if (!assigned) presets.presets[String(S.status.slot || 1)] = S.editing;  // to hear it while editing
  }
  return presets;
}

let previewTimer = null, previewBusy = false, previewAgain = false;
function schedulePreview(delay = 120) {
  clearTimeout(previewTimer);
  previewTimer = setTimeout(sendPreview, delay);
}
async function sendPreview() {
  if (previewBusy) { previewAgain = true; return; }
  previewBusy = true;
  const r = await api("/api/preview", { programs: S.draft.programs, presets: previewPresets() });
  previewBusy = false;
  if (r.ok) setWarnings(r.warnings || []);
  else if (!r.offline) setWarnings([r.error, ...(r.warnings || [])]);
  if (previewAgain) { previewAgain = false; sendPreview(); }
}

function setWarnings(list) {
  S.warnings = list.filter(Boolean);
  const box = document.querySelector("#liveWarnings");
  if (box) box.innerHTML = warningsHtml();
}
const warningsHtml = () => S.warnings.length
  ? `<ul class="warnings">${S.warnings.map((w) => `<li>${esc(w)}</li>`).join("")}</ul>` : "";

async function save() {
  const r = await api("/api/save", S.draft);
  if (r.ok) {
    S.saved = clone(S.draft);
    updateDirty();
    setWarnings(r.warnings || []);
    toast(`Salvato. Copia dei file precedenti: <b>${esc(r.backup)}</b>` + (r.warnings && r.warnings.length ? `<br>${r.warnings.length} avvisi` : ""));
    refreshBank();
  } else {
    toast("Non salvato: " + esc(r.error) + (r.warnings ? "<br>" + r.warnings.map(esc).join("<br>") : ""), true, 8000);
  }
}

async function revert() {
  if (!confirm("Tornare alla configurazione salvata? Le modifiche andranno perse.")) return;
  S.draft = clone(S.saved);
  await api("/api/revert", {});
  setWarnings([]);
  updateDirty();
  refreshBank();
  render();
}

async function refreshBank() {
  const st = await api("/api/state");
  if (st.bank && st.bank.ok) S.bank = st.bank;
  S.backups = st.backups || S.backups;
  if (S.tab !== "sounds" || !S.editing) render();
}

// ---------------------------------------------------------------------------
// Status bar
// ---------------------------------------------------------------------------
async function pollStatus() {
  const st = await api("/api/status");
  $("#offlineBar").hidden = !st.offline;
  if (st.ok) {
    const prev = S.status;
    S.status = st;
    $("#stProgram").textContent = st.program ? `${st.slot} · ${st.program.name || st.program.id}` : "—";
    $("#stVoices").textContent = `${st.voices} v`;
    $("#stLoad").textContent = `${Math.round(st.load_avg)}%`;
    const p = $("#stPeak");
    const pct = Math.max(0, Math.min(100, (st.peak_db + 40) / 40 * 100));
    p.style.width = pct + "%";
    p.className = st.limiter_db < -0.05 ? "clip" : st.peak_db > -3 ? "hot" : "";
    if (st.midi_messages !== S.midiSeen) {
      S.midiSeen = st.midi_messages;
      const led = $("#midiLed");
      led.classList.add("on");
      setTimeout(() => led.classList.remove("on"), 120);
    }
    // Engine restarted or reloaded while we have unsaved edits: play them again.
    if (isDirty() && !st.edited && !previewBusy) schedulePreview(0);
    if (S.tab === "panel" && (prev.slot !== st.slot || prev.switch !== st.switch || prev.has_switch !== st.has_switch)) renderPanelLive();
    if (S.tab === "system") renderSystemLive();
  }
  setTimeout(pollStatus, st.ok ? 600 : 1500);
}

// ---------------------------------------------------------------------------
// Views
// ---------------------------------------------------------------------------
function render() {
  document.querySelectorAll(".tabs button").forEach((b) => b.classList.toggle("on", b.dataset.tab === S.tab));
  const v = $("#view");
  if (!S.draft) { v.innerHTML = `<p class="muted">Caricamento…</p>`; return; }
  updateDirty();
  if (S.tab === "panel") renderPanel(v);
  else if (S.tab === "sounds") (S.editing ? renderEditor(v) : renderSounds(v));
  else if (S.tab === "instruments") renderInstruments(v);
  else renderSystem(v);
  showKeyboard(S.tab === "sounds" && S.editing && S.kbOpen !== false);
}

const programs = () => S.draft.programs.programs || (S.draft.programs.programs = []);
const findProgram = (id) => programs().find((p) => p.id === id);
const available = (id) => !S.bank || S.bank.programs.some((p) => p.id === id);

// --- Panel -----------------------------------------------------------------
function renderPanel(v) {
  const pr = S.draft.presets;
  const pad = (i) => {
    const id = pr.presets[String(i)];
    const p = id ? findProgram(id) : null;
    const cls = ["pad"];
    if (!id) cls.push("empty");
    else if (!p || !available(id)) cls.push("missing");
    if (S.status.slot === i) cls.push("on");
    return `<button class="${cls.join(" ")}" data-slot="${i}">
      <span class="n">${i}</span>${pr.startup === i ? '<span class="star">★</span>' : ""}
      <span class="name">${id ? esc(p ? p.name || p.id : id) : S.panelMode === "assign" ? "+" : "vuoto"}</span></button>`;
  };
  // Rows of 8 up to the last defined preset; while assigning, one more row to add presets.
  const last = Math.max(0, ...Object.keys(pr.presets).map(Number));
  let rows = Math.max(1, Math.ceil(last / ROW));
  if (S.panelMode === "assign") rows = Math.min(rows + 1, Math.ceil(PRESET_MAX / ROW));
  const grid = Array.from({ length: rows }, (_, r) =>
    `<div class="grid">${Array.from({ length: ROW }, (_, k) => r * ROW + k + 1).filter((n) => n <= PRESET_MAX).map(pad).join("")}</div>`).join("");
  v.innerHTML = `
    <div class="row-title"><h2>Preset</h2>
      <div class="mode"><button class="small ${S.panelMode === "play" ? "on" : ""}" data-mode="play">Suona</button>
      <button class="small ${S.panelMode === "assign" ? "on" : ""}" data-mode="assign">Assegna</button></div></div>
    ${grid}
    ${S.panelMode === "assign" ? `<p class="muted small-text">Tocca un tasto per scegliere il suono; l'ultima fila serve ad aggiungere preset. ★ = parte all'accensione. Il preset N si richiama con il Program Change N−1.</p>` : ""}
    <div class="controls">
      <button class="big" id="fxSwitch">Switch effetti</button>
      <button class="big danger" id="panic">Panic</button>
      <div class="card wide">${sliderField("Volume generale", "master", S.status.volume_db ?? -6, { minimum: -40, maximum: 6 }, "gain_db")}
      <div class="muted small-text">Non salvato: all'accensione vale <i>master_volume_db</i> di engine.json.</div></div>
    </div>`;
  v.querySelectorAll(".pad").forEach((b) => b.onclick = () => {
    const slot = +b.dataset.slot;
    if (S.panelMode === "assign") return assignSheet(slot);
    if (!pr.presets[String(slot)]) return;
    cmd({ cmd: "preset", slot });
    S.status.slot = slot;
    renderPanelLive();
  });
  v.querySelectorAll("[data-mode]").forEach((b) => b.onclick = () => { S.panelMode = b.dataset.mode; render(); });
  $("#panic").onclick = () => cmd({ cmd: "panic" });
  $("#fxSwitch").onclick = () => cmd({ cmd: "switch" });
  bindSlider(v.querySelector('[data-key="master"]'), (val) => cmd({ cmd: "volume", db: val }));
  renderPanelLive();
}
function renderPanelLive() {
  document.querySelectorAll(".pad").forEach((b) => b.classList.toggle("on", +b.dataset.slot === S.status.slot));
  const sw = $("#fxSwitch");
  if (sw) {
    sw.disabled = !S.status.has_switch;
    sw.textContent = S.status.has_switch ? `Switch effetti: ${S.status.switch ? "B" : "A"}` : "Switch effetti (—)";
    sw.classList.toggle("switch-b", !!S.status.switch);
  }
}

function assignSheet(slot) {
  const pr = S.draft.presets;
  const current = pr.presets[String(slot)];
  const opts = programs().map((p) => `<button class="opt ${p.id === current ? "on" : ""}" data-id="${esc(p.id)}">
      ${esc(p.name || p.id)} <span class="muted small-text">${esc(p.id)}${available(p.id) ? "" : " · non disponibile"}</span></button>`).join("");
  openSheet(`<h3>Preset ${slot}</h3>
    <button class="opt ${!current ? "on" : ""}" data-id="">— vuoto —</button>${opts}
    <label class="toggle">Parte all'accensione <input type="checkbox" id="startup" ${pr.startup === slot ? "checked" : ""}></label>`);
  const sheet = $("#sheet");
  sheet.querySelectorAll(".opt").forEach((b) => b.onclick = () => {
    setPreset(slot, b.dataset.id || null);
    closeSheet(); changed(true);
  });
  sheet.querySelector("#startup").onchange = (e) => { if (e.target.checked) { pr.startup = slot; changed(true); } };
}

function openSheet(html) {
  $("#sheet").innerHTML = html; $("#sheet").hidden = false; $("#sheetBg").hidden = false;
}
function closeSheet() { $("#sheet").hidden = true; $("#sheetBg").hidden = true; }

// --- Sounds list -------------------------------------------------------------
function slotsOf(id) {
  return Object.entries(S.draft.presets.presets).filter(([, v]) => v === id).map(([k]) => k);
}
function renderSounds(v) {
  const items = programs().map((p) => {
    const slots = slotsOf(p.id);
    const layers = (p.layers || []).map((l) => l.instrument).join(" + ");
    const fx = (p.fx || []).filter((f) => !f.bypass).map((f) => FX_NAMES[f.type] || f.type).join(" › ");
    return `<div class="item" data-id="${esc(p.id)}"><div class="main">
      <div class="title">${esc(p.name || p.id)}</div>
      <div class="sub">${esc(layers)}${fx ? " · " + esc(fx) : ""}</div></div>
      ${available(p.id) ? "" : '<span class="badge warn">non disponibile</span>'}
      ${slots.length ? `<span class="badge">preset ${slots.join(", ")}</span>` : ""}</div>`;
  }).join("");
  v.innerHTML = `<h2>Suoni (${programs().length})</h2><div class="list">${items}</div>
    <div class="actions"><button class="primary" id="newProgram">+ Nuovo suono</button></div>`;
  v.querySelectorAll(".item").forEach((it) => it.onclick = () => openEditor(it.dataset.id));
  $("#newProgram").onclick = newProgram;
}

function uniqueId(base) {
  let id = base, n = 2;
  while (findProgram(id)) id = `${base}_${n++}`;
  return id;
}
function newProgram() {
  const inst = (S.loaded[0] && S.loaded[0].id) || (S.instrumentsDef[0] && S.instrumentsDef[0].id) || "";
  const id = uniqueId("nuovo");
  programs().push({ id, name: "Nuovo suono", layers: [{ instrument: inst }] });
  openEditor(id);
  changed();
}

function openEditor(id) {
  S.editing = id;
  S.tab = "sounds";
  const slots = slotsOf(id);
  if (isDirty() || !slots.length) schedulePreview(0);  // the draft, with this program on the current button if it has none
  if (slots.length) {
    cmd({ cmd: "preset", slot: +slots[0] });
    S.status.slot = +slots[0];
  }
  render();
  window.scrollTo(0, 0);
}

// --- Program editor ------------------------------------------------------------
function schemaProgram() {
  try { return S.schema.programs.properties.programs.items; } catch (e) { return null; }
}
function fxSchema(type) {
  const items = schemaProgram()?.properties?.fx?.items;
  const block = (items?.allOf || []).find((b) => b.if?.properties?.type?.const === type);
  return block?.then?.properties?.params?.properties || {};
}
function fxTypes() {
  return schemaProgram()?.properties?.fx?.items?.properties?.type?.enum || Object.keys(FX_NAMES);
}

function renderEditor(v) {
  const p = findProgram(S.editing);
  if (!p) { S.editing = null; return renderSounds(v); }
  const sp = schemaProgram()?.properties || {};
  const slots = slotsOf(p.id);
  v.innerHTML = `
    <div class="ed-head"><button class="ghost small" id="back">‹ Suoni</button>
      <h1>${esc(p.name || p.id)}</h1>
      <button class="ghost small" id="kbToggle">Tastiera</button></div>
    <div class="muted small-text">${slots.length ? "Preset " + slots.join(", ") : "Non è su nessun preset: mentre lo modifichi suona sul preset attuale"}${available(p.id) ? "" : " · <span style='color:var(--bad)'>non disponibile nel motore</span>"}</div>
    <div id="liveWarnings">${warningsHtml()}</div>

    <h2>Generale</h2>
    <div class="card">
      <div class="field"><label>Nome</label><input type="text" id="pName" value="${esc(p.name || "")}"></div>
      <div class="field"><label>ID <span class="muted small-text">(nei file e nei preset)</span></label><input type="text" id="pId" value="${esc(p.id)}"></div>
      ${sliderField("Volume", "gain_db", p.gain_db, sp.gain_db || { minimum: -60, maximum: 12, default: 0 }, "gain_db", { min: -30 })}
      ${segField("Uscita (DAC)", "output", p.output || "A", ["A", "B", "AB"])}
    </div>

    <h2>Inviluppo e dinamica</h2>
    <div class="card" id="pEnv">${envelopeHtml(p, sp)}</div>

    <h2>Layer</h2>
    <div id="layers">${(p.layers || []).map((l, i) => layerHtml(l, i, p)).join("")}</div>
    ${(p.layers || []).length < 3 ? `<button id="addLayer">+ Layer</button>` : ""}

    <h2>Effetti</h2>
    <div id="fxList">${(p.fx || []).map((f, i) => fxHtml(f, i, p)).join("")}</div>
    <div class="two"><select id="fxType">${fxTypes().map((t) => `<option value="${t}">${FX_NAMES[t] || t}</option>`).join("")}</select>
      <button id="addFx">+ Effetto</button></div>

    <div class="actions">
      <button id="dup">Duplica</button>
      <button class="danger" id="del">Elimina suono</button>
    </div>`;

  $("#back").onclick = () => { S.editing = null; render(); if (isDirty()) schedulePreview(0); };
  $("#kbToggle").onclick = () => { S.kbOpen = $("#keyboard").hidden; showKeyboard(S.kbOpen); };
  $("#pName").oninput = (e) => { p.name = e.target.value; v.querySelector("h1").textContent = p.name || p.id; changed(); };
  $("#pId").onchange = (e) => renameProgram(p, e.target.value.trim());
  bindSlider(v.querySelector('[data-key="gain_db"]'), (val) => { setOrDelete(p, "gain_db", val, 0); changed(); });
  bindSeg(v.querySelector('[data-key="output"]'), (val) => { if (val === "A") delete p.output; else p.output = val; changed(); });
  bindEnvelope(v.querySelector("#pEnv"), p, false);
  (p.layers || []).forEach((l, i) => bindLayer(v.querySelector(`[data-layer="${i}"]`), l, i, p));
  (p.fx || []).forEach((f, i) => bindFx(v.querySelector(`[data-fx="${i}"]`), f, i, p));
  const addLayer = $("#addLayer");
  if (addLayer) addLayer.onclick = () => { p.layers.push({ instrument: p.layers[0]?.instrument || S.loaded[0]?.id || "" }); changed(true); };
  $("#addFx").onclick = () => { (p.fx = p.fx || []).push({ type: $("#fxType").value }); changed(true); };
  $("#dup").onclick = () => {
    const copy = clone(p);
    copy.id = uniqueId(p.id + "_copia");
    copy.name = (p.name || p.id) + " (copia)";
    programs().splice(programs().indexOf(p) + 1, 0, copy);
    openEditor(copy.id); changed();
  };
  $("#del").onclick = () => {
    if (!confirm(`Eliminare "${p.name || p.id}"? Viene tolto anche dai preset.`)) return;
    programs().splice(programs().indexOf(p), 1);
    for (const k of Object.keys(S.draft.presets.presets)) if (S.draft.presets.presets[k] === p.id) setPreset(+k, null);
    S.editing = null; changed(true);
  };
}

function renameProgram(p, id) {
  if (!id || id === p.id) return;
  if (!/^[A-Za-z0-9_\-]+$/.test(id)) { toast("ID: solo lettere, numeri, _ e -", true); return render(); }
  if (findProgram(id)) { toast("Esiste già un suono con questo ID", true); return render(); }
  for (const k of Object.keys(S.draft.presets.presets)) if (S.draft.presets.presets[k] === p.id) S.draft.presets.presets[k] = id;
  p.id = id; S.editing = id; changed(true);
}

function setOrDelete(obj, key, val, def) {
  if (val === undefined || val === def) delete obj[key]; else obj[key] = val;
}

// ADSR + velocity + loop, for the program (inherit = false) or a layer (inherit = true).
function envelopeHtml(o, sp, inherit = false) {
  const adsrS = sp.adsr?.properties || {};
  const velS = sp.velocity?.properties || {};
  const own = (k) => o[k] !== undefined;
  const adsr = o.adsr || {};
  const vel = o.velocity || {};
  const loopVal = o.loop === true || o.loop === "on" ? "on" : o.loop === false || o.loop === "off" ? "off" : undefined;
  const block = (k, title, body) => inherit
    ? `<label class="toggle">${title} proprio <input type="checkbox" data-own="${k}" ${own(k) ? "checked" : ""}></label>${own(k) ? `<div class="fold">${body}</div>` : ""}`
    : `<div class="lbl">${title}</div>${body}`;
  const adsrBody = ["attack_ms", "decay_ms", "sustain", "release_ms"]
    .map((k) => sliderField(LABELS[k], `adsr.${k}`, adsr[k], adsrS[k] || {}, k)).join("");
  const velBody = segField("Velocity", "velocity.mode", vel.mode ?? "off", ["off", "on"])
    + (vel.mode === "on" ? sliderField(LABELS.levels, "velocity.levels", vel.levels, velS.levels || { minimum: 1, maximum: 127, default: 127, type: "integer" }, "levels")
      + segField(LABELS.curve, "velocity.curve", vel.curve ?? "linear", ["linear", "exp"]) : "");
  const loopBody = segField("Organ mode (loop)", "loop", loopVal ?? "off", ["off", "on"]);
  return block("adsr", "Inviluppo", adsrBody) + block("velocity", "Velocity", velBody) + block("loop", "Loop", loopBody);
}

function bindEnvelope(el, o, inherit) {
  el.querySelectorAll("[data-own]").forEach((cb) => cb.onchange = () => {
    const k = cb.dataset.own;
    if (cb.checked) o[k] = k === "loop" ? "off" : {}; else delete o[k];
    changed(true);
  });
  el.querySelectorAll(".field[data-key]").forEach((f) => {
    const key = f.dataset.key;
    const [group, k] = key.split(".");
    if (f.dataset.kind === "slider") {
      bindSlider(f, (val, isDefault) => {
        o[group] = o[group] || {};
        if (isDefault) delete o[group][k]; else o[group][k] = val;
        if (!inherit && !Object.keys(o[group]).length) delete o[group];
        changed();
      });
    } else {
      bindSeg(f, (val) => {
        if (group === "loop") { o.loop = val; if (!inherit && val === "off") delete o.loop; }
        else {
          o[group] = o[group] || {};
          o[group][k] = val;
          if (group === "velocity" && k === "mode" && val === "off" && !inherit) delete o.velocity;
        }
        changed(true);
      });
    }
  });
}

function instrumentOptions(selected) {
  const ids = new Set();
  const opts = [];
  for (const d of S.instrumentsDef) {
    ids.add(d.id);
    const loaded = S.loaded.find((l) => l.id === d.id);
    opts.push(`<option value="${esc(d.id)}" ${d.id === selected ? "selected" : ""}>${esc(d.name || d.id)}${loaded ? "" : " (non caricato)"}</option>`);
  }
  for (const l of S.loaded) if (!ids.has(l.id)) opts.push(`<option value="${esc(l.id)}" ${l.id === selected ? "selected" : ""}>${esc(l.name || l.id)}</option>`);
  if (selected && !ids.has(selected) && !S.loaded.some((l) => l.id === selected)) opts.push(`<option selected>${esc(selected)}</option>`);
  return opts.join("");
}

function layerHtml(l, i, p) {
  const sp = schemaProgram()?.properties?.layers?.items?.properties || {};
  const lo = l.keys ? noteNumber(l.keys[0]) : 0, hi = l.keys ? noteNumber(l.keys[1]) : 127;
  const noteOpts = (sel) => Array.from({ length: 128 }, (_, n) => `<option value="${n}" ${n === sel ? "selected" : ""}>${noteName(n)}</option>`).join("");
  return `<div class="card" data-layer="${i}">
    <div class="layer-head"><span class="t">Layer ${i + 1}</span>
      ${p.layers.length > 1 ? `<button class="small ghost danger" data-act="remove">Togli</button>` : ""}</div>
    <div class="field"><label>Strumento</label><select data-f="instrument">${instrumentOptions(l.instrument)}</select></div>
    <div class="field"><div class="lbl">Tasti <span class="val">${l.keys ? noteName(lo) + " – " + noteName(hi) : "tutta la tastiera"}</span></div>
      <div class="two"><select data-f="lo">${noteOpts(lo)}</select><select data-f="hi">${noteOpts(hi)}</select></div></div>
    ${sliderField("Trasposizione", "transpose", l.transpose, sp.transpose || { minimum: -48, maximum: 48, default: 0, type: "integer" }, "transpose", { min: -24, max: 24 })}
    ${sliderField("Volume", "gain_db", l.gain_db, sp.gain_db || { minimum: -60, maximum: 12, default: 0 }, "gain_db", { min: -30 })}
    <div class="fold">${envelopeHtml(l, sp, true)}</div>
  </div>`;
}

function bindLayer(el, l, i, p) {
  el.querySelector('[data-f="instrument"]').onchange = (e) => { l.instrument = e.target.value; changed(); };
  const lo = el.querySelector('[data-f="lo"]'), hi = el.querySelector('[data-f="hi"]');
  const keys = () => {
    let a = +lo.value, b = +hi.value;
    if (a > b) [a, b] = [b, a];
    if (a === 0 && b === 127) delete l.keys; else l.keys = [noteName(a), noteName(b)];
    changed(true);
  };
  lo.onchange = keys; hi.onchange = keys;
  bindSlider(el.querySelector('[data-key="transpose"]'), (val) => { setOrDelete(l, "transpose", val, 0); changed(); });
  bindSlider(el.querySelector('[data-key="gain_db"]'), (val) => { setOrDelete(l, "gain_db", val, 0); changed(); });
  const rm = el.querySelector('[data-act="remove"]');
  if (rm) rm.onclick = () => { p.layers.splice(i, 1); changed(true); };
  bindEnvelope(el.querySelector(".fold"), l, true);
}

function fxHtml(f, i, p) {
  const params = fxSchema(f.type);
  const vals = f.params || {};
  const body = Object.entries(params).map(([k, s]) => paramField(k, s, vals[k])).join("");
  const sw = f.switch || null;
  const swOpts = ["", "bypass", ...Object.keys(params)].map((k) =>
    `<option value="${k}" ${sw && sw[0] === k ? "selected" : !sw && !k ? "selected" : ""}>${k ? LABELS[k] || k : "nessuno"}</option>`).join("");
  return `<div class="card fx ${f.bypass ? "bypassed" : ""}" data-fx="${i}">
    <div class="fx-head"><span class="t">${FX_NAMES[f.type] || f.type}</span>
      <button class="small ghost" data-act="up" ${i === 0 ? "disabled" : ""}>▲</button>
      <button class="small ghost" data-act="down" ${i === p.fx.length - 1 ? "disabled" : ""}>▼</button>
      <button class="small ghost danger" data-act="remove">✕</button></div>
    <label class="toggle">Escluso (bypass) <input type="checkbox" data-act="bypass" ${f.bypass ? "checked" : ""}></label>
    <div class="fx-body">${body}</div>
    <div class="fold"><div class="field"><label>Switch (tasto effetti): parametro</label><select data-act="swparam">${swOpts}</select></div>
      ${sw ? `<div class="two">${switchValue(sw[0], params, sw[1], "A")}${switchValue(sw[0], params, sw[2], "B")}</div>` : ""}</div>
  </div>`;
}

function switchValue(key, params, value, which) {
  const s = key === "bypass" ? { type: "boolean" } : params[key] || {};
  let input;
  if (s.enum) input = `<select data-sw="${which}">${s.enum.map((e) => `<option value="${e}" ${e === value ? "selected" : ""}>${ENUM_LABELS[e] || e}</option>`).join("")}</select>`;
  else if (s.type === "boolean") input = `<select data-sw="${which}" data-bool="1"><option value="false" ${value === false ? "selected" : ""}>${key === "bypass" ? "attivo" : "no"}</option><option value="true" ${value === true ? "selected" : ""}>${key === "bypass" ? "escluso" : "sì"}</option></select>`;
  else input = `<input type="number" data-sw="${which}" step="any" min="${s.minimum ?? ""}" max="${s.maximum ?? ""}" value="${value ?? s.default ?? 0}">`;
  return `<div class="field"><label>${which === "A" ? "A (all'avvio)" : "B (premuto)"}</label>${input}</div>`;
}

function paramField(k, s, value) {
  if (s.enum && s.type !== "integer") return segField(LABELS[k] || k, k, value ?? s.default, s.enum);
  if (s.enum) return segField(LABELS[k] || k, k, value ?? s.default, s.enum);
  if (s.type === "boolean") return `<label class="toggle field" data-key="${k}" data-kind="bool">${LABELS[k] || k}<input type="checkbox" ${(value ?? s.default) ? "checked" : ""}></label>`;
  return sliderField(LABELS[k] || k, k, value, s, k);
}

function bindFx(el, f, i, p) {
  const act = (a) => el.querySelector(`[data-act="${a}"]`);
  act("up").onclick = () => { [p.fx[i - 1], p.fx[i]] = [p.fx[i], p.fx[i - 1]]; changed(true); };
  act("down").onclick = () => { [p.fx[i + 1], p.fx[i]] = [p.fx[i], p.fx[i + 1]]; changed(true); };
  act("remove").onclick = () => { p.fx.splice(i, 1); if (!p.fx.length) delete p.fx; changed(true); };
  act("bypass").onchange = (e) => { if (e.target.checked) f.bypass = true; else delete f.bypass; changed(true); };
  const params = fxSchema(f.type);
  el.querySelectorAll(".fx-body .field[data-key]").forEach((fld) => {
    const k = fld.dataset.key;
    const set = (val, isDefault) => {
      f.params = f.params || {};
      if (isDefault) delete f.params[k]; else f.params[k] = val;
      if (!Object.keys(f.params).length) delete f.params;
      changed();
    };
    if (fld.dataset.kind === "slider") bindSlider(fld, set);
    else if (fld.dataset.kind === "bool") fld.querySelector("input").onchange = (e) => set(e.target.checked, e.target.checked === !!params[k]?.default);
    else bindSeg(fld, (val) => { set(isNaN(+val) || typeof params[k]?.default === "string" ? val : +val, false); render(); });
  });
  act("swparam").onchange = (e) => {
    const k = e.target.value;
    if (!k) { delete f.switch; return changed(true); }
    const s = k === "bypass" ? { type: "boolean", default: false } : params[k];
    let a = k === "bypass" ? false : (f.params?.[k] ?? s.default), b = a;
    if (k === "bypass") b = true;
    else if (s.enum) b = s.enum.find((x) => x !== a) ?? a;
    else if (s.type === "boolean") b = !a;
    else b = s.maximum !== undefined ? Math.min(s.maximum, (a || 0) * 2 || s.maximum) : a;
    f.switch = [k, a, b];
    changed(true);
  };
  el.querySelectorAll("[data-sw]").forEach((inp) => inp.onchange = () => {
    const idx = inp.dataset.sw === "A" ? 1 : 2;
    let val = inp.value;
    if (inp.dataset.bool) val = val === "true";
    else if (inp.type === "number") val = +val;
    else if (!isNaN(+val) && typeof params[f.switch[0]]?.default === "number") val = +val;
    f.switch[idx] = val;
    changed();
  });
}

// --- Generic controls -----------------------------------------------------------
// Sliders: frequencies and times move on a logarithmic scale.
function scaleFor(s, hint) {
  const min = s.minimum ?? 0, max = s.maximum ?? 1;
  const unit = hint.endsWith("_hz") ? "hz" : hint.endsWith("_ms") ? "ms" : hint.endsWith("_s") ? "s" : hint.endsWith("_db") ? "db" : hint === "transpose" ? "st" : "";
  const floor = { hz: 20, ms: 1, s: 0.05 }[unit];
  const lo = min > 0 ? min : floor;
  const log = floor !== undefined && max / lo >= 20;
  return { min, max, unit, log, lo, integer: s.type === "integer" || hint === "transpose" || hint === "levels" };
}
function toPos(sc, v) {
  if (!sc.log) return Math.round((v - sc.min) / (sc.max - sc.min) * 1000);
  if (v <= sc.min && sc.min === 0) return 0;
  return Math.round(Math.log(Math.max(v, sc.lo) / sc.lo) / Math.log(sc.max / sc.lo) * (sc.min === 0 ? 990 : 1000)) + (sc.min === 0 ? 10 : 0);
}
function fromPos(sc, pos) {
  let v;
  if (!sc.log) v = sc.min + (sc.max - sc.min) * pos / 1000;
  else if (sc.min === 0 && pos < 10) v = 0;
  else v = sc.lo * Math.pow(sc.max / sc.lo, (sc.min === 0 ? (pos - 10) / 990 : pos / 1000));
  return roundValue(sc, v);
}
function roundValue(sc, v) {
  if (sc.integer) return Math.round(v);
  if (sc.unit === "hz") return v >= 1000 ? Math.round(v / 50) * 50 : v >= 100 ? Math.round(v / 5) * 5 : Math.round(v);
  if (sc.unit === "ms") return v >= 100 ? Math.round(v / 5) * 5 : Math.round(v * 10) / 10;
  if (sc.unit === "db") return Math.round(v * 2) / 2;
  return Math.round(v * 100) / 100;
}
function formatValue(sc, v) {
  if (v === undefined || v === null) return "";
  if (sc.unit === "hz") return v === 0 && sc.min === 0 ? "spento" : v >= 1000 ? (v / 1000).toFixed(v >= 10000 ? 1 : 2) + " kHz" : Math.round(v) + " Hz";
  if (sc.unit === "ms") return v >= 1000 ? (v / 1000).toFixed(2) + " s" : (v < 10 ? v.toFixed(1) : Math.round(v)) + " ms";
  if (sc.unit === "s") return v.toFixed(2) + " s";
  if (sc.unit === "db") return (v > 0 ? "+" : "") + v.toFixed(1) + " dB";
  if (sc.unit === "st") return (v > 0 ? "+" : "") + v + " semitoni";
  if (sc.integer) return String(v);
  return v.toFixed(2);
}

// limits: optional narrower slider range ({min, max}); typed values may still use the full range.
function sliderField(label, key, value, s, hint, limits = {}) {
  const sc = scaleFor(Object.assign({}, s, { minimum: limits.min ?? s.minimum, maximum: limits.max ?? s.maximum }), hint);
  const def = s.default ?? (sc.min <= 0 && sc.max >= 0 ? 0 : sc.min);
  const v = value ?? def;
  const isDef = value === undefined;
  return `<div class="field" data-key="${key}" data-kind="slider" data-scale='${JSON.stringify(sc)}' data-default="${def}">
    <div class="lbl">${esc(label)}<span><span class="val ${isDef ? "default" : ""}">${formatValue(sc, v)}</span>
      ${isDef ? "" : `<button class="reset" title="valore predefinito">↺</button>`}</span></div>
    <input type="range" min="0" max="1000" value="${toPos(sc, Math.min(sc.max, Math.max(sc.min, v)))}"></div>`;
}
function bindSlider(field, onChange) {
  if (!field) return;
  const sc = JSON.parse(field.dataset.scale);
  const def = +field.dataset.default;
  const input = field.querySelector("input");
  const val = field.querySelector(".val");
  let last = null;
  input.oninput = () => {
    const v = fromPos(sc, +input.value);
    if (v === last) return;
    last = v;
    val.textContent = formatValue(sc, v);
    val.classList.remove("default");
    onChange(v, false);
  };
  val.onclick = () => {
    const typed = prompt("Valore", last ?? val.textContent.replace(/[^\d.+-]/g, ""));
    if (typed === null || typed.trim() === "" || isNaN(+typed)) return;
    const v = +typed;
    last = v;
    val.textContent = formatValue(sc, v);
    input.value = toPos(sc, Math.min(sc.max, Math.max(sc.min, v)));
    onChange(v, false);
  };
  const reset = field.querySelector(".reset");
  if (reset) reset.onclick = () => { onChange(def, true); render(); };
}
function segField(label, key, value, options) {
  return `<div class="field" data-key="${key}" data-kind="seg"><div class="lbl">${esc(label)}</div><div class="seg">
    ${options.map((o) => `<button class="${String(o) === String(value) ? "on" : ""}" data-v="${o}">${ENUM_LABELS[o] || o}</button>`).join("")}</div></div>`;
}
function bindSeg(field, onChange) {
  field.querySelectorAll("button").forEach((b) => b.onclick = () => {
    field.querySelectorAll("button").forEach((x) => x.classList.toggle("on", x === b));
    onChange(b.dataset.v);
  });
}

// --- Instruments ----------------------------------------------------------------
function renderInstruments(v) {
  const rows = S.instrumentsDef.map((d) => {
    const l = S.loaded.find((x) => x.id === d.id);
    return `<div class="item"><div class="main"><div class="title">${esc(d.name || d.id)}</div>
      <div class="sub">${esc(d.id)} · ${esc(d.path || "")}${l ? ` · ${l.files} file · ${l.mb} MB${l.loops ? " · loop" : ""}` : ""}</div></div>
      ${l ? '<span class="badge ok">in memoria</span>' : '<span class="badge warn">non caricato</span>'}</div>`;
  }).join("");
  const total = S.loaded.reduce((a, l) => a + l.mb, 0);
  v.innerHTML = `<h2>Strumenti (${S.loaded.length} caricati, ${total.toFixed(0)} MB)</h2><div class="list">${rows}</div>
    <p class="muted small-text">Gli strumenti si aggiungono in <i>instruments.json</i> copiando i campioni nella cartella media; il motore li carica all'avvio.</p>`;
}

// --- System -----------------------------------------------------------------------
function renderSystem(v) {
  v.innerHTML = `<h2>Registrazione</h2><div class="card" id="recCard"><span class="muted">…</span></div>
    <h2>Motore</h2><div class="card" id="sysLive"></div>
    <h2>Avvisi</h2><div class="card" id="sysWarn"><span class="muted">…</span></div>
    <h2>Copie di sicurezza</h2><div class="card">
      <p class="muted small-text">A ogni salvataggio i file precedenti vanno in <i>config/backup</i> (ultime 30).</p>
      ${S.backups.length ? `<select id="bk">${S.backups.map((b) => `<option>${esc(b)}</option>`).join("")}</select>
      <div class="actions"><button id="restore">Ripristina questa copia</button></div>` : '<span class="muted">Nessuna copia ancora.</span>'}
    </div>
    ${S.hadComments.length ? `<p class="muted small-text">I file ${S.hadComments.join(", ")} contengono commenti //: salvando dal web vengono riscritti senza commenti (resta la copia).</p>` : ""}`;
  renderSystemLive();
  renderRecording();
  api("/api/warnings").then((w) => {
    const list = w.active || [...(w.startup || []), ...(w.last || [])];  // the ones in force now
    $("#sysWarn").innerHTML = list.length ? `<ul class="warnings">${list.map((x) => `<li>${esc(x)}</li>`).join("")}</ul>` : '<span class="muted">Nessun avviso.</span>';
  });
  const r = $("#restore");
  if (r) r.onclick = async () => {
    const b = $("#bk").value;
    if (!confirm(`Ripristinare la copia ${b}? La configurazione attuale viene salvata prima in un'altra copia.`)) return;
    const res = await api("/api/restore", { backup: b });
    if (!res.ok) return toast("Ripristino fallito: " + esc(res.error), true);
    S.draft = null; await loadState(); toast("Copia ripristinata");
  };
}
// --- Recording: what the box plays, as a WAV in the Pi's RAM (/tmp), to download.
let recTimer = null;
const mmss = (t) => `${Math.floor(t / 60)}:${String(Math.floor(t % 60)).padStart(2, "0")}`;
async function renderRecording() {
  const card = $("#recCard");
  if (!card) return;
  const [st, list] = await Promise.all([cmd({ cmd: "record", action: "status" }), api("/api/recordings")]);
  if (!$("#recCard")) return;
  const files = (list.files || []).map((f) => `<div class="item"><div class="main"><div class="title">${esc(f.name)}</div>
      <div class="sub">${mmss(f.seconds)} · ${f.mb} MB</div></div>
      <a class="badge ok" href="/api/recordings/${encodeURIComponent(f.name)}" download>scarica</a>
      <button class="small ghost danger" data-del="${esc(f.name)}">✕</button></div>`).join("");
  card.innerHTML = `
    <button class="big ${st.recording ? "switch-b" : ""}" id="recBtn" style="width:100%">${st.recording ? `■ Ferma (${mmss(st.seconds)})` : "● Registra"}</button>
    ${st.failed ? '<p class="warnings">Registrazione interrotta: spazio finito o errore di scrittura.</p>' : ""}
    <p class="muted small-text">Stereo, 16 bit, quello che esce dalla scatola. I file stanno in ${esc(list.folder || "/tmp")}
      (in RAM col sistema in sola lettura: circa 5 minuti, e spariscono allo spegnimento).</p>
    <div class="list">${files || '<span class="muted">Nessuna registrazione.</span>'}</div>`;
  $("#recBtn").onclick = async () => {
    const r = await cmd({ cmd: "record", action: st.recording ? "stop" : "start" });
    if (!r.ok) toast("Registrazione: " + esc(r.error), true);
    renderRecording();
  };
  card.querySelectorAll("[data-del]").forEach((b) => b.onclick = async () => {
    if (!confirm(`Cancellare ${b.dataset.del}?`)) return;
    await api("/api/recordings/delete", { name: b.dataset.del });
    renderRecording();
  });
  clearTimeout(recTimer);
  if (st.recording) recTimer = setTimeout(renderRecording, 1000);
}

function renderSystemLive() {
  const el = $("#sysLive");
  if (!el) return;
  const s = S.status;
  const up = s.uptime_s ? `${Math.floor(s.uptime_s / 3600)}h ${Math.floor(s.uptime_s / 60) % 60}m` : "—";
  el.innerHTML = `<div class="two small-text">
    <div>Versione <b>${esc(s.version || "—")}</b></div><div>Acceso da <b>${up}</b></div>
    <div>Voci <b>${s.voices ?? 0}</b> / ${s.polyphony ?? "—"} (max ${s.max_voices ?? 0})</div>
    <div>CPU audio <b>${s.load_avg ?? 0}%</b> (max ${s.load_max ?? 0}%)</div>
    <div>Picco <b>${s.peak_db > -100 ? s.peak_db + " dBFS" : "silenzio"}</b>${s.limiter_db < -0.05 ? ` · limiter ${s.limiter_db} dB` : ""}</div>
    <div>Temperatura <b>${s.temperature ?? "—"} °C</b></div>
    <div>Messaggi MIDI <b>${s.midi_messages ?? 0}</b></div><div>Eventi persi <b>${s.dropped ?? 0}</b></div></div>`;
}

// ---------------------------------------------------------------------------
// Keyboard (to try a sound without the real one)
// ---------------------------------------------------------------------------
function showKeyboard(on) {
  $("#keyboard").hidden = !on;
  document.body.classList.toggle("kb-open", !!on);
  if (on) buildKeys();
}
function buildKeys() {
  const keys = $("#keys");
  const span = window.innerWidth >= 700 ? 24 : 14;  // white keys shown
  const whites = [], blacks = [];
  let n = S.kbBase;
  while (whites.length < span) {
    if ([1, 3, 6, 8, 10].includes(n % 12)) blacks.push({ n, after: whites.length }); else whites.push(n);
    n++;
  }
  const w = 100 / whites.length;
  keys.innerHTML = whites.map((k) => `<div class="key" data-n="${k}"></div>`).join("") +
    blacks.map((b) => `<div class="key black" data-n="${b.n}" style="left:${b.after * w - w * 0.32}%;width:${w * 0.64}%"></div>`).join("");
  $("#kbOct").textContent = noteName(S.kbBase);
}
const held = new Map();  // pointerId -> note
function keyAt(x, y) {
  const el = document.elementFromPoint(x, y);
  return el && el.classList.contains("key") ? el : null;
}
function press(id, el) {
  const n = +el.dataset.n;
  if (held.get(id) === n) return;
  release(id);
  held.set(id, n);
  el.classList.add("down");
  cmd({ cmd: "note", key: n, velocity: +$("#kbVel").value });
}
function release(id) {
  const n = held.get(id);
  if (n === undefined) return;
  held.delete(id);
  document.querySelectorAll(`.key[data-n="${n}"]`).forEach((k) => k.classList.remove("down"));
  cmd({ cmd: "note", key: n, velocity: 0 });
}
function setupKeyboard() {
  const keys = $("#keys");
  keys.addEventListener("pointerdown", (e) => { const k = keyAt(e.clientX, e.clientY); if (k) { keys.setPointerCapture(e.pointerId); press(e.pointerId, k); } e.preventDefault(); });
  keys.addEventListener("pointermove", (e) => { if (!held.has(e.pointerId)) return; const k = keyAt(e.clientX, e.clientY); if (k) press(e.pointerId, k); });
  for (const ev of ["pointerup", "pointercancel"]) keys.addEventListener(ev, (e) => release(e.pointerId));
  $("#kbDown").onclick = () => { S.kbBase = Math.max(0, S.kbBase - 12); buildKeys(); };
  $("#kbUp").onclick = () => { S.kbBase = Math.min(108, S.kbBase + 12); buildKeys(); };
  $("#kbHide").onclick = () => { S.kbOpen = false; showKeyboard(false); };
}

// ---------------------------------------------------------------------------
// Start
// ---------------------------------------------------------------------------
document.querySelectorAll(".tabs button").forEach((b) => b.onclick = () => {
  S.tab = b.dataset.tab;
  if (S.tab !== "sounds") S.editing = null;
  render();
  window.scrollTo(0, 0);
});
$("#btnSave").onclick = save;
$("#btnRevert").onclick = revert;
$("#sheetBg").onclick = closeSheet;
window.addEventListener("beforeunload", (e) => { if (isDirty()) { e.preventDefault(); e.returnValue = ""; } });
setupKeyboard();
loadState();
pollStatus();
