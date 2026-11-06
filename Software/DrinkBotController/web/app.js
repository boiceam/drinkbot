/* DrinkBotController web app (SPEC_v3 §14, §15). Vanilla JS, no external resources. */
'use strict';
(function () {

// ------------------------------------------------------------------ helpers
const $ = (s, r) => (r || document).querySelector(s);
const $$ = (s, r) => Array.from((r || document).querySelectorAll(s));
const clone = (o) => (o === undefined ? undefined : JSON.parse(JSON.stringify(o)));
const isNum = (v) => typeof v === 'number' && isFinite(v);
const isInt = (v) => isNum(v) && Math.floor(v) === v;
const fmt1 = (v) => (isNum(v) ? v.toFixed(1) : '—');
const esc = (s) => String(s == null ? '' : s).replace(/[&<>"']/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]));

function el(tag, attrs) {
  const e = document.createElement(tag);
  if (attrs) for (const k in attrs) {
    const v = attrs[k];
    if (v == null || v === false) continue;
    if (k === 'class') e.className = v;
    else if (k === 'text') e.textContent = v;
    else if (k === 'html') e.innerHTML = v;
    else if (k.startsWith('on')) e.addEventListener(k.slice(2), v);
    else if (k === 'dataset') Object.assign(e.dataset, v);
    else if (k in e && typeof v !== 'string') e[k] = v;
    else e.setAttribute(k, v === true ? '' : v);
  }
  for (let i = 2; i < arguments.length; i++) {
    const kids = [].concat(arguments[i]);
    for (const c of kids) if (c != null && c !== false) e.append(c.nodeType ? c : document.createTextNode(String(c)));
  }
  return e;
}

// Parse "pumps[0].msPerMl" (or "pumps.0.msPerMl") into keys.
function pathKeys(p) {
  return String(p).replace(/\[(\d+)\]/g, '.$1').split('.').filter((k) => k !== '').map((k) => (/^\d+$/.test(k) ? +k : k));
}
function normPath(p) { return pathKeys(p).reduce((a, k) => (typeof k === 'number' ? a + '[' + k + ']' : (a ? a + '.' : '') + k), ''); }
function getPath(o, p) { return pathKeys(p).reduce((a, k) => (a == null ? undefined : a[k]), o); }
function setPath(o, p, v) {
  const ks = pathKeys(p);
  let a = o;
  for (let i = 0; i < ks.length - 1; i++) {
    if (a[ks[i]] == null) a[ks[i]] = typeof ks[i + 1] === 'number' ? [] : {};
    a = a[ks[i]];
  }
  a[ks[ks.length - 1]] = v;
}

function rgbHex(c) { return '#' + (c || [0, 0, 0]).map((x) => ('0' + ((+x || 0) & 255).toString(16)).slice(-2)).join(''); }
function hexRgb(h) { const n = parseInt(String(h).slice(1), 16); return [(n >> 16) & 255, (n >> 8) & 255, n & 255]; }
function slugify(s) { return String(s || '').toLowerCase().trim().replace(/[^a-z0-9]+/g, '-').replace(/^-+|-+$/g, '').slice(0, 32) || 'recipe'; }

function fwOld(v) {
  if (!v) return false;
  const m = /^(\d+)\.(\d+)\.(\d+)/.exec(v);
  if (!m) return false;
  const a = [+m[1], +m[2], +m[3]], b = [0, 3, 0];
  for (let i = 0; i < 3; i++) if (a[i] !== b[i]) return a[i] < b[i];
  return false;
}

const SEVERITY = {
  E_USB_LOST: 'blocking', E_BRIDGE_UNRESPONSIVE: 'blocking',
  E_PUMP_MISSING: 'degraded', E_PUMP_FW: 'degraded', E_CONFIG: 'degraded',
  E_DISPENSE_TIMEOUT: 'latched', E_PUMP_COMM: 'latched', E_STOP_UNCONFIRMED: 'latched', E_PUMP_UPDATE: 'latched',
};
const BULK_GAP_MS = 500;

function toast(msg, kind, ms) {
  const t = el('div', { class: 'toast ' + (kind || ''), text: msg });
  $('#toasts').append(t);
  setTimeout(() => t.remove(), ms || (kind === 'err' ? 6000 : 3000));
}

// ------------------------------------------------------------------ app state
const S = {
  connected: false, hello: null, state: null, pumps: [], config: null,
  disp: null, bulk: null, tab: 'dash',
};
function pumpRt(id) { return S.pumps.find((p) => p.pumpId === id); }
function cfgPump(id) { return S.config ? S.config.pumps.find((p) => p.pumpId === id) : null; }
function errorsOf(sev) { return ((S.state && S.state.errors) || []).filter((e) => (SEVERITY[e.code] || (e.latched ? 'latched' : 'degraded')) === sev); }
function sys() { return S.state ? S.state.system : 'BOOTING'; }
function isBusy() { return sys() === 'DISPENSING' || sys() === 'MANUAL' || sys() === 'BOOTING'; }
function canManual() { return S.connected && !isBusy() && !errorsOf('blocking').length; }
function canDispense() { return canManual() && !errorsOf('latched').length; }
function activeVolume() { return S.state && isNum(S.state.activeVolumeMl) ? S.state.activeVolumeMl : (S.config ? S.config.defaults.volumeMl : 250); }

// ------------------------------------------------------------------ WebSocket
let ws = null, attempt = 0, reqSeq = 0;
const pending = new Map();
const BACKOFF = [1000, 2000, 4000, 5000];

function connect() {
  const url = (location.protocol === 'https:' ? 'wss://' : 'ws://') + (location.host || 'drinkbot.local') + '/ws';
  try { ws = new WebSocket(url); } catch (e) { scheduleReconnect(); return; }
  ws.onopen = () => { attempt = 0; setConnected(true); };
  ws.onclose = () => {
    ws = null;
    setConnected(false);
    for (const [, p] of pending) { clearTimeout(p.timer); p.reject({ code: 'DISCONNECTED', message: 'Connection lost' }); }
    pending.clear();
    scheduleReconnect();
  };
  ws.onerror = () => { /* onclose follows */ };
  ws.onmessage = (ev) => {
    let m;
    try { m = JSON.parse(ev.data); } catch (e) { return; }
    try { onMessage(m); } catch (e) { console.warn('handler failed for', m && m.type, e); }
  };
}
function scheduleReconnect() { setTimeout(connect, BACKOFF[Math.min(attempt++, BACKOFF.length - 1)]); }

function send(type, fields) {
  return new Promise((resolve, reject) => {
    if (!ws || ws.readyState !== 1) { reject({ code: 'DISCONNECTED', message: 'Not connected' }); return; }
    const reqId = 'r' + (++reqSeq);
    const timer = setTimeout(() => { pending.delete(reqId); reject({ code: 'TIMEOUT', message: 'No reply from controller' }); }, 20000);
    pending.set(reqId, { resolve, reject, timer, type });
    ws.send(JSON.stringify(Object.assign({ type, reqId }, fields || {})));
  });
}
// send + toast on failure. Resolves to ack data, or undefined on error.
function cmd(type, fields, okMsg) {
  return send(type, fields).then((d) => { if (okMsg) toast(okMsg, 'ok'); return d === undefined ? true : d; },
    (e) => { toast(type + ': ' + (e.code ? e.code + ' — ' : '') + (e.message || 'failed'), 'err'); return undefined; });
}

const handlers = {
  hello(m) { S.hello = m; S.manual = S.manualReq = null; $('#simBanner').hidden = !m.simulator; $('#simPanel').hidden = !m.simulator; document.title = 'DrinkBot — ' + (m.hostname || ''); renderInfo(); },
  state(m) { S.state = m; if (m.system !== 'MANUAL') S.manual = S.manualReq = null; renderHeader(); renderDash(); renderMaint(); applyEnabled(); },
  pumps(m) { S.pumps = m.pumps || []; renderDash(); updatePumpStatusCells(); renderMaint(); applyEnabled(); },
  config(m) { onConfig(m.config || m); },
  log(m) { logAdd([m]); },
  logHistory(m) { logHistory(m.lines || []); },
  dispenseProgress(m) { onDispenseProgress(m); },
  bulkProgress(m) { onBulkProgress(m); },
  ota(m) { onOta(m); },
  pumpFw(m) { S.pumpFw = m; renderPumpFw(); },
  ack(m) {
    const p = pending.get(m.reqId);
    if (p) { pending.delete(m.reqId); clearTimeout(p.timer); p.resolve(m.data); }
  },
  error(m) {
    const p = m.reqId && pending.get(m.reqId);
    if (p) { pending.delete(m.reqId); clearTimeout(p.timer); p.reject(m); }
    else toast('Controller: ' + m.code + ' — ' + m.message, 'err');
  },
};
function onMessage(m) { const h = handlers[m.type]; if (h) h(m); }

// ------------------------------------------------------------------ enable/disable
// Elements with data-needs-conn are disabled while disconnected; renderers can add
// their own reason via setOff(el, bool).
function setOff(e, off) { if (e) { e.dataset.off = off ? '1' : ''; applyOne(e); } }
function applyOne(e) {
  const dis = !S.connected || e.dataset.off === '1';
  if (e.tagName === 'LABEL') { e.classList.toggle('disabled', dis); const i = $('input', e); if (i) i.disabled = dis; }
  else e.disabled = dis;
}
function applyEnabled() { $$('[data-needs-conn]').forEach(applyOne); renderPumpFw(); }

function setConnected(on) {
  S.connected = on;
  const c = $('#conn');
  c.textContent = on ? 'Connected' : 'Reconnecting';
  c.className = 'conn ' + (on ? 'on' : 'off');
  applyEnabled();
  renderDash();
}

// ------------------------------------------------------------------ header
function renderHeader() {
  const st = S.state;
  if (!st) return;
  const w = st.wifi || {};
  let wt = 'Wi-Fi: ' + (w.mode || '—');
  if (w.ssid) wt += ' ' + w.ssid;
  if (w.ip) wt += ' ' + w.ip;
  if (w.mode === 'STA' && isNum(w.rssi)) wt += ' (' + w.rssi + ' dBm)';
  $('#wifi').textContent = wt;
  const p = $('#sysState');
  p.textContent = '● ' + st.system;
  p.className = 'pill ' + st.system;
  const errs = st.errors || [];
  const bar = $('#errBar');
  bar.hidden = !errs.length;
  bar.innerHTML = errs.map((e) => '<b>' + esc(e.code) + (e.pumpId != null ? '(' + esc(e.pumpId) + ')' : '') + '</b> ' + esc(e.message) +
    (e.latched ? ' <span class="muted">[latched]</span>' : '')).join(' · ');
}

$('#btnEstop').addEventListener('click', () => {
  send('estop').then(() => toast('E-STOP sent', 'ok'), (e) => toast('E-STOP failed: ' + (e.message || e.code), 'err'));
});
$('#btnReboot').addEventListener('click', async () => {
  const busy = sys() === 'DISPENSING' || sys() === 'MANUAL';
  const ok = await confirmDlg('Reboot controller?', busy
    ? 'An operation is running (' + sys() + '). It will be E-Stopped, then the controller reboots.'
    : 'The controller restarts and the page reconnects when it is back.', busy ? 'E-Stop and reboot' : 'Reboot', true);
  if (ok) cmd('reboot', busy ? { force: true } : {}, 'Rebooting…');
});

// ------------------------------------------------------------------ modal
let modalResolve = null;
function confirmDlg(title, body, okLabel, danger) {
  if (modalResolve) modalResolve(false);
  $('#modalTitle').textContent = title;
  const b = $('#modalBody');
  b.innerHTML = '';
  if (typeof body === 'string') b.append(el('p', { text: body })); else if (body) b.append(body);
  const ok = $('#modalOk');
  ok.textContent = okLabel || 'OK';
  ok.className = 'btn ' + (danger ? 'danger' : 'primary');
  $('#modal').hidden = false;
  ok.focus();
  return new Promise((res) => { modalResolve = res; });
}
function closeModal(v) { $('#modal').hidden = true; if (modalResolve) { const r = modalResolve; modalResolve = null; r(v); } }
$('#modalOk').addEventListener('click', () => closeModal(true));
$('#modalCancel').addEventListener('click', () => closeModal(false));
$('#modal').addEventListener('click', (e) => { if (e.target.id === 'modal') closeModal(false); });
document.addEventListener('keydown', (e) => { if (e.key === 'Escape' && !$('#modal').hidden) closeModal(false); });

// ------------------------------------------------------------------ log panel
const LOG_MAX = 2000;
const logView = $('#logView');
let logCount = 0, lastLogT = -1, logQueue = [], logRaf = 0;
function pad(n, w) { n = String(n); while (n.length < w) n = ' ' + n; return n; }
function padR(s, w) { s = String(s || ''); while (s.length < w) s += ' '; return s; }
function logLine(l) { return '[' + pad(l.t, 8) + '] ' + padR(l.level, 5) + ' ' + padR(l.tag, 4) + ' - ' + l.msg; }
function logAdd(lines) {
  for (const l of lines) { if (isNum(l.t)) lastLogT = l.t; logQueue.push(l); }
  if (logQueue.length > LOG_MAX) logQueue.splice(0, logQueue.length - LOG_MAX);
  if (!logRaf) logRaf = setTimeout(logFlush, 50); // timer, not rAF: keeps working in background tabs
}
function logFlush() {
  logRaf = 0;
  const q = logQueue.splice(0, logQueue.length).slice(-LOG_MAX);
  const frag = document.createDocumentFragment();
  for (const l of q) frag.append(el('div', { class: l.level || '', text: l.sep ? l.msg : logLine(l) }));
  logView.append(frag);
  logCount += q.length;
  while (logCount > LOG_MAX) { logView.firstChild.remove(); logCount--; }
  $('#logCount').textContent = logCount + ' lines';
  if ($('#logAuto').checked) { logView.scrollTop = logView.scrollHeight; }
}
function logHistory(lines) {
  if (lastLogT < 0) { logAdd(lines); return; }
  const lastHist = lines.length ? lines[lines.length - 1].t : 0;
  if (lastHist < lastLogT) { // controller restarted: uptime went backwards
    logAdd([{ sep: true, level: 'WARN', msg: '──────── controller restarted ────────' }]);
    lastLogT = -1;
    logAdd(lines);
  } else logAdd(lines.filter((l) => l.t > lastLogT));
}
logView.addEventListener('scroll', () => {
  const atBottom = logView.scrollTop + logView.clientHeight >= logView.scrollHeight - 4;
  $('#logAuto').checked = atBottom;
});
$('#logAuto').addEventListener('change', (e) => { if (e.target.checked) logView.scrollTop = logView.scrollHeight; });
$('#logClear').addEventListener('click', () => { logView.innerHTML = ''; logCount = 0; $('#logCount').textContent = ''; });
$('#logLevel').addEventListener('change', (e) => cmd('setLogLevel', { level: e.target.value }, 'Web log level: ' + e.target.value));

// Resizable log panel (80 px .. 70% of the window), height kept in localStorage.
const LOG_KEY = 'drinkbot.logHeight';
function setLogH(h, save) {
  h = Math.round(Math.max(80, Math.min(window.innerHeight * 0.7, h)));
  document.documentElement.style.setProperty('--log-h', h + 'px');
  if (save) { try { localStorage.setItem(LOG_KEY, String(h)); } catch (e) { /* storage unavailable */ } }
  return h;
}
let logH = 200;
try { const v = parseInt(localStorage.getItem(LOG_KEY), 10); if (v) logH = v; } catch (e) { /* ignore */ }
logH = setLogH(logH, false);
(function () {
  const r = $('#logResize');
  let startY = 0, startH = 0, dragging = false;
  r.addEventListener('pointerdown', (e) => { dragging = true; startY = e.clientY; startH = $('#log').getBoundingClientRect().height; r.setPointerCapture(e.pointerId); e.preventDefault(); });
  r.addEventListener('pointermove', (e) => { if (dragging) logH = setLogH(startH + (startY - e.clientY), false); });
  const end = () => { if (dragging) { dragging = false; logH = setLogH(logH, true); } };
  r.addEventListener('pointerup', end);
  r.addEventListener('pointercancel', end);
  window.addEventListener('resize', () => { logH = setLogH(logH, false); });
})();

// ------------------------------------------------------------------ validation (mirrors §7.3)
function trimAll(o) {
  if (Array.isArray(o)) return o.map(trimAll);
  if (o && typeof o === 'object') { const r = {}; for (const k in o) r[k] = trimAll(o[k]); return r; }
  return typeof o === 'string' ? o.trim() : o;
}
const utf8Len = (s) => new TextEncoder().encode(s).length;

function validateConfig(c) {
  const errs = [];
  const E = (path, message) => errs.push({ path, message });
  const range = (path, v, lo, hi, int) => { if (!(int ? isInt(v) : isNum(v)) || v < lo || v > hi) E(path, (int ? 'integer ' : '') + lo + '–' + hi); };

  const dev = c.device || {};
  if (!/^[A-Za-z0-9-]{1,32}$/.test(dev.hostname || '')) E('device.hostname', '1–32 letters, digits or “-”');
  if (!dev.apSsid || utf8Len(dev.apSsid) > 32) E('device.apSsid', '1–32 bytes');

  const nets = (c.wifi && c.wifi.networks) || [];
  if (nets.length > 8) E('wifi.networks', 'at most 8 networks');
  nets.forEach((n, i) => {
    const l = utf8Len(n.ssid || '');
    if (l < 1 || l > 32) E('wifi.networks[' + i + '].ssid', '1–32 bytes');
    const pw = n.password || '';
    if (pw && (pw.length < 8 || pw.length > 63)) E('wifi.networks[' + i + '].password', 'empty (open) or 8–63 characters');
  });

  const lv = c.levels || {};
  let lvOk = true;
  ['lowMl', 'emptyMl', 'actualEmptyMl'].forEach((k) => { if (!isNum(lv[k])) { E('levels.' + k, 'must be a number'); lvOk = false; } });
  if (lvOk) {
    if (lv.actualEmptyMl < 0) E('levels.actualEmptyMl', 'must be ≥ 0');
    if (!(lv.actualEmptyMl < lv.emptyMl)) E('levels.emptyMl', 'must be greater than “actually empty”');
    if (!(lv.emptyMl < lv.lowMl)) E('levels.lowMl', 'must be greater than “indicated empty”');
  }

  const d = c.dispense || {};
  range('dispense.verifyTolerancePct', d.verifyTolerancePct, 0, 100);
  range('dispense.verifyToleranceMinMl', d.verifyToleranceMinMl, 0, 1000);
  range('dispense.settleMs', d.settleMs, 0, 10000, true);
  range('dispense.minVolumeMl', d.minVolumeMl, 1, 1000);
  range('dispense.maxVolumeMl', d.maxVolumeMl, 1, 1000);
  const vmin = isNum(d.minVolumeMl) ? d.minVolumeMl : 10, vmax = isNum(d.maxVolumeMl) ? d.maxVolumeMl : 1000;
  if (isNum(d.minVolumeMl) && isNum(d.maxVolumeMl) && vmin >= vmax) E('dispense.maxVolumeMl', 'must be greater than the minimum');

  range('ui.ledBrightness', (c.ui || {}).ledBrightness, 0, 255, true);

  const pumps = c.pumps || [];
  if (pumps.length > 16) E('pumps', 'at most 16 pumps');
  const ids = {}, liqs = {};
  pumps.forEach((p, i) => {
    const P = 'pumps[' + i + ']';
    if (!isInt(p.pumpId) || p.pumpId < 1 || p.pumpId > 254) E(P + '.pumpId', 'integer 1–254');
    else if (ids[p.pumpId]) E(P + '.pumpId', 'duplicate pump ID ' + p.pumpId); else ids[p.pumpId] = 1;
    const liq = p.liquid || '';
    if (liq.length > 32) E(P + '.liquid', 'at most 32 characters');
    if (liq) { const k = liq.toLowerCase(); if (liqs[k]) E(P + '.liquid', 'duplicate liquid “' + liq + '”'); liqs[k] = 1; }
    range(P + '.msPerMl', p.msPerMl, 1, 65535, true);
    range(P + '.densityGPerMl', p.densityGPerMl, 0.5, 2.0);
    if (!Array.isArray(p.color) || p.color.length !== 3 || !p.color.every((x) => isInt(x) && x >= 0 && x <= 255)) E(P + '.color', '3 integers 0–255');
    const a = p.autoPrimeSec;
    if (!isNum(a) || a < 0 || a > 60 || Math.abs(a * 10 - Math.round(a * 10)) > 1e-6) E(P + '.autoPrimeSec', '0–60 s in 0.1 s steps');
  });

  const recipes = c.recipes || [];
  if (recipes.length > 64) E('recipes', 'at most 64 recipes');
  const rids = {};
  recipes.forEach((r, i) => {
    const R = 'recipes[' + i + ']';
    if (!/^[a-z0-9-]{1,32}$/.test(r.id || '')) E(R + '.id', '1–32 of a–z, 0–9, “-”');
    else if (rids[r.id]) E(R + '.id', 'duplicate id “' + r.id + '”');
    rids[r.id] = 1;
    if (!r.name || r.name.length > 32) E(R + '.name', '1–32 characters');
    const ings = r.ingredients || [];
    if (ings.length < 1 || ings.length > 16) E(R + '.ingredients', '1–16 ingredients');
    ings.forEach((g, j) => {
      const G = R + '.ingredients[' + j + ']';
      if (!g.liquid || g.liquid.length > 32) E(G + '.liquid', '1–32 characters');
      if (!isNum(g.parts) || !(g.parts > 0)) E(G + '.parts', 'must be > 0');
      if (!isInt(g.order) || g.order < 0) E(G + '.order', 'integer ≥ 0');
    });
  });

  const btns = c.buttons || [];
  if (btns.length !== 7) E('buttons', 'exactly 7 buttons');
  btns.forEach((b, i) => {
    const B = 'buttons[' + i + ']';
    if (['none', 'setRecipe', 'setVolume', 'dispense', 'estop'].indexOf(b.action) < 0) E(B + '.action', 'unknown action');
    else if (b.action === 'setRecipe' && !rids[b.recipeId]) E(B + '.recipeId', 'recipe does not exist');
    else if (b.action === 'setVolume' && (!isNum(b.volumeMl) || b.volumeMl < vmin || b.volumeMl > vmax)) E(B + '.volumeMl', vmin + '–' + vmax + ' ml');
  });

  const df = c.defaults || {};
  if (!isNum(df.volumeMl) || df.volumeMl < vmin || df.volumeMl > vmax) E('defaults.volumeMl', vmin + '–' + vmax + ' ml');
  if (df.recipeId && !rids[df.recipeId]) E('defaults.recipeId', 'recipe does not exist');
  range('defaults.primeSec', df.primeSec, 1, 60);
  range('defaults.purgeSec', df.purgeSec, 1, 60);
  return errs;
}
// Accept details as [{path,message}], ["path: message"], or {path: message}.
function normErrors(details) {
  if (!details) return [];
  if (!Array.isArray(details)) return Object.keys(details).map((k) => ({ path: normPath(k), message: String(details[k]) }));
  return details.map((d) => {
    if (typeof d === 'string') { const i = d.indexOf(':'); return i > 0 ? { path: normPath(d.slice(0, i).trim()), message: d.slice(i + 1).trim() } : { path: normPath(d), message: 'invalid' }; }
    return { path: normPath(d.path || d.field || ''), message: d.message || d.msg || 'invalid' };
  });
}

// ------------------------------------------------------------------ editors
// Each editor keeps a full-config draft and owns some top-level sections of it.
const editors = {};
function defEditor(name, sections, render) {
  editors[name] = { name, sections, render, draft: null, dirty: false, errors: [], sec: $('#tab-' + name) };
}
function resetEditor(ed) { ed.draft = clone(S.config); ed.dirty = false; ed.errors = []; ed.render(ed); showDirty(ed); showErrors(ed); }
function markDirty(ed) { if (!ed.dirty) { ed.dirty = true; showDirty(ed); } }
function showDirty(ed) {
  const d = $('.dirty', ed.sec); if (d) d.hidden = !ed.dirty;
  const b = $('.tabbar [data-tab="' + ed.name + '"]');
  if (b) { const dot = $('.dot', b); if (ed.dirty && !dot) b.append(el('span', { class: 'dot', text: '●' })); if (!ed.dirty && dot) dot.remove(); }
}
function showErrors(ed) {
  $$('.invalid', ed.sec).forEach((e) => e.classList.remove('invalid'));
  $$('.ferr', ed.sec).forEach((e) => e.remove());
  const sum = $('.errsum', ed.sec);
  if (!ed.errors.length) { if (sum) sum.hidden = true; return; }
  for (const e of ed.errors) {
    const f = $$('[data-path]', ed.sec).find((x) => x.dataset.path === e.path);
    if (f) { f.classList.add('invalid'); f.insertAdjacentElement('afterend', el('span', { class: 'ferr', text: e.message })); }
  }
  if (sum) {
    sum.hidden = false;
    sum.innerHTML = '';
    sum.append(el('strong', { text: 'Not saved — fix ' + ed.errors.length + ' problem' + (ed.errors.length > 1 ? 's' : '') + ':' }),
      el('ul', null, ed.errors.map((e) => el('li', null, el('code', { text: e.path || '(config)' }), ' ', e.message))));
  }
  if (ed.onErrors) ed.onErrors(ed);
}
function buildFull(ed) {
  const c = clone(S.config);
  for (const s of ed.sections) c[s] = clone(ed.draft[s]);
  return trimAll(c);
}
async function saveEditor(ed) {
  if (!S.config) return;
  const cfg = buildFull(ed);
  ed.errors = validateConfig(cfg);
  showErrors(ed);
  if (ed.errors.length) { toast('Not saved: ' + ed.errors.length + ' validation error(s)', 'err'); return; }
  ed.saving = true;
  try {
    await send('saveConfig', { config: cfg });
    ed.saving = false;
    ed.dirty = false;
    resetEditor(ed);
    const reboot = ed.sections.indexOf('wifi') >= 0 || (ed.sections.indexOf('device') >= 0 &&
      JSON.stringify(cfg.device) !== JSON.stringify(S.lastSavedDevice || cfg.device));
    toast('Saved' + (reboot ? '. Wi-Fi / hostname / AP changes apply after reboot.' : ''), 'ok', reboot ? 6000 : 3000);
  } catch (e) {
    ed.saving = false;
    ed.errors = normErrors(e.details);
    if (!ed.errors.length) ed.errors = [{ path: '', message: (e.code || '') + ' ' + (e.message || 'save failed') }];
    showErrors(ed);
    toast('Save rejected: ' + (e.message || e.code), 'err');
  }
}
function onConfig(cfg) {
  const first = !S.config;
  S.lastSavedDevice = S.config ? clone(S.config.device) : clone(cfg.device);
  S.config = cfg;
  for (const k in editors) {
    const ed = editors[k];
    if (!ed.dirty) resetEditor(ed);
    else if (!first && !ed.saving) toast('The config changed on the controller. Your unsaved ' + k + ' edits are kept.', '', 5000);
  }
  renderDash(true);
  renderMaint();
  applyEnabled();
}
// Generic field binding: inputs with data-path write into the editor draft.
function readField(e) {
  const t = e.dataset.type || (e.type === 'number' ? 'num' : 'str');
  if (t === 'color') return hexRgb(e.value);
  if (t === 'num' || t === 'int') { if (e.value === '') return null; const v = Number(e.value); return isFinite(v) ? v : null; }
  if (t === 'bool') return e.checked;
  return e.value;
}
function bindEditor(ed) {
  const h = (ev) => {
    const f = ev.target.closest('[data-path]');
    if (!f || !ed.draft || f.readOnly) return;
    setPath(ed.draft, f.dataset.path, readField(f));
    markDirty(ed);
    if (ed.onField) ed.onField(ed, f, ev);
  };
  ed.sec.addEventListener('input', h);
  ed.sec.addEventListener('change', (ev) => { if (ed.onCommit && ev.target.tagName === 'INPUT') ed.onCommit(ed, ev.target); });
  $$('[data-save]', ed.sec).forEach((b) => b.addEventListener('click', () => saveEditor(ed)));
  $$('[data-revert]', ed.sec).forEach((b) => b.addEventListener('click', async () => {
    if (!ed.dirty || await confirmDlg('Discard changes?', 'Your unsaved edits on this tab will be lost.', 'Discard', true)) resetEditor(ed);
  }));
}
function inp(path, value, attrs) {
  const a = Object.assign({ dataset: { path } }, attrs || {});
  if (a.type === 'number' || a.dataset.type === 'num' || a.dataset.type === 'int') { a.type = 'number'; a.value = value == null ? '' : String(value); }
  else if (a.dataset.type === 'color') { a.type = 'color'; a.value = rgbHex(value); }
  else a.value = value == null ? '' : String(value);
  if (attrs && attrs.dataset) a.dataset = Object.assign({ path }, attrs.dataset);
  return el('input', a);
}

// ------------------------------------------------------------------ tabs
async function showTab(name) {
  if (name === S.tab) return;
  const cur = editors[S.tab];
  if (cur && cur.dirty) {
    const ok = await confirmDlg('Leave with unsaved changes?', 'The ' + $('.tabbar [data-tab="' + S.tab + '"]').firstChild.textContent + ' tab has unsaved edits. Discard them?', 'Discard', true);
    if (!ok) { $('#tabSelect').value = S.tab; return; }
    resetEditor(cur);
  }
  S.tab = name;
  $$('.tabbar button').forEach((b) => b.classList.toggle('active', b.dataset.tab === name));
  $$('.tab').forEach((t) => t.classList.toggle('active', t.id === 'tab-' + name));
  $('#tabSelect').value = name;
  try { history.replaceState(null, '', '#' + name); } catch (e) { /* ignore */ }
  if (name === 'settings') fetchInfo();
  if (name === 'dash') renderDash(true);
}
$$('.tabbar button').forEach((b) => b.addEventListener('click', () => showTab(b.dataset.tab)));
$('#tabSelect').addEventListener('change', (e) => showTab(e.target.value));
window.addEventListener('beforeunload', (e) => {
  if (Object.keys(editors).some((k) => editors[k].dirty)) { e.preventDefault(); e.returnValue = ''; }
});

// ------------------------------------------------------------------ recipe math
function recipeSteps(r, vol) {
  const ings = (r.ingredients || []).map((g, i) => ({ g, i })).sort((a, b) => (a.g.order - b.g.order) || (a.i - b.i)).map((x) => x.g);
  const tot = ings.reduce((s, g) => s + (isNum(g.parts) ? g.parts : 0), 0);
  return ings.map((g) => ({ liquid: g.liquid, ml: tot > 0 ? Math.round(vol * g.parts / tot * 10) / 10 : 0 }));
}
// '' if the recipe can be poured at vol, else the reason.
function recipeProblem(r, vol) {
  if (!S.config) return '';
  const need = {};
  for (const s of recipeSteps(r, vol)) {
    const p = S.config.pumps.find((x) => x.liquid && s.liquid && x.liquid.toLowerCase() === s.liquid.toLowerCase());
    if (!p) return s.liquid + ' is not loaded on any pump';
    const rt = pumpRt(p.pumpId);
    if (rt && !rt.present) return s.liquid + ': pump ' + p.pumpId + ' is missing';
    if (rt && fwOld(rt.fwVersion)) return s.liquid + ': pump ' + p.pumpId + ' firmware too old';
    need[p.pumpId] = (need[p.pumpId] || 0) + s.ml;
  }
  for (const id in need) {
    const rt = pumpRt(+id);
    if (rt && isNum(rt.levelMl) && rt.levelMl - need[id] < S.config.levels.actualEmptyMl) return 'Not enough ' + cfgPump(+id).liquid + ' (' + Math.round(rt.levelMl) + ' ml left)';
  }
  return '';
}

// ------------------------------------------------------------------ dashboard
let recipeSig = '';
function renderDash(force) {
  if (!S.config) return;
  const cfg = S.config, st = S.state || {};
  const vol = activeVolume();
  // recipe selector
  const sel = $('#dRecipe');
  const probs = cfg.recipes.map((r) => recipeProblem(r, vol));
  const sig = JSON.stringify([cfg.recipes.map((r) => r.id + r.name), probs, st.activeRecipeId]);
  if (force || sig !== recipeSig) {
    recipeSig = sig;
    sel.innerHTML = '';
    sel.append(el('option', { value: '', text: '— none —' }));
    cfg.recipes.forEach((r, i) => sel.append(el('option', { value: r.id, text: r.name + (probs[i] ? ' (unavailable)' : ''), title: probs[i] || null, class: probs[i] ? 'unavail' : null, style: probs[i] ? 'color:#888' : null })));
    sel.value = st.activeRecipeId || '';
  }
  const vi = $('#dVolume');
  vi.min = cfg.dispense.minVolumeMl; vi.max = cfg.dispense.maxVolumeMl;
  if (document.activeElement !== vi) vi.value = vol;
  const rec = cfg.recipes.find((r) => r.id === st.activeRecipeId);
  const prob = rec ? recipeProblem(rec, vol) : 'Choose a recipe';
  $('#dRecipeInfo').textContent = rec ? (prob ? '⚠ ' + prob : 'At ' + vol + ' ml: ' + recipeSteps(rec, vol).map((s) => s.liquid + ' ' + fmt1(s.ml) + ' ml').join(', ')) : '';
  setOff($('#dGo'), !rec || !canDispense());
  $('#dGo').title = !rec ? 'Choose a recipe' : (prob || '');
  renderLeds();
  renderBulkBar();
  renderPumpCards();
}
$('#dRecipe').addEventListener('change', (e) => cmd('setRecipe', { recipeId: e.target.value }));
$('#dVolume').addEventListener('change', (e) => {
  const v = Number(e.target.value), d = S.config.dispense;
  if (!isFinite(v) || v < d.minVolumeMl || v > d.maxVolumeMl) { toast('Volume must be ' + d.minVolumeMl + '–' + d.maxVolumeMl + ' ml', 'err'); e.target.value = activeVolume(); return; }
  cmd('setVolume', { volumeMl: v });
});
$('#dGo').addEventListener('click', () => {
  const v = Number($('#dVolume').value);
  cmd('dispense', { recipeId: $('#dRecipe').value, volumeMl: v });
});

function onDispenseProgress(m) {
  let D = S.disp;
  if (!D || D.final || D.recipeId !== m.recipeId) {
    const r = S.config && S.config.recipes.find((x) => x.id === m.recipeId);
    D = S.disp = { recipeId: m.recipeId, name: r ? r.name : m.recipeId, volumeMl: m.volumeMl, steps: m.steps, final: false, rows: {} };
    if (r) recipeSteps(r, m.volumeMl).forEach((s, i) => { D.rows[i + 1] = { liquid: s.liquid, targetMl: s.ml, status: 'PENDING' }; });
  }
  D.steps = m.steps || D.steps;
  if (m.status === 'COMPLETE' || m.status === 'ABORTED') {
    D.final = true; D.status = m.status; D.reason = m.reason;
    if (m.status === 'ABORTED' && D.rows[m.step] && D.rows[m.step].status !== 'DONE') D.rows[m.step].status = 'ABORTED';
  } else {
    D.rows[m.step] = Object.assign(D.rows[m.step] || {}, { liquid: m.liquid, pumpId: m.pumpId, targetMl: m.targetMl, actualMl: m.actualMl, status: m.status });
  }
  renderDispProgress();
}
function renderDispProgress() {
  const D = S.disp;
  $('#dProgress').hidden = !D;
  if (!D) return;
  $('#dProgTitle').textContent = D.name + ' — ' + D.volumeMl + ' ml';
  const tol = S.config ? S.config.dispense : { verifyTolerancePct: 15, verifyToleranceMinMl: 5 };
  let done = 0;
  const body = $('#dProgRows');
  body.innerHTML = '';
  for (let i = 1; i <= D.steps; i++) {
    const r = D.rows[i] || { status: 'PENDING' };
    let sym = '', cls = 'muted';
    if (r.status === 'DONE') {
      done++;
      const dev = isNum(r.actualMl) ? Math.abs(r.actualMl - r.targetMl) : 0;
      const bad = dev > Math.max(r.targetMl * tol.verifyTolerancePct / 100, tol.verifyToleranceMinMl);
      sym = bad ? '⚠' : '✓'; cls = bad ? 'st-warn' : 'st-ok';
    } else if (r.status === 'STARTING') { sym = '…'; cls = 'st-run'; }
    else if (r.status === 'ABORTED') { sym = '✗'; cls = 'st-err'; }
    body.append(el('tr', null, el('td', { text: i }), el('td', { text: r.liquid || '' }), el('td', { text: fmt1(r.targetMl) + ' ml' }),
      el('td', { text: isNum(r.actualMl) ? fmt1(r.actualMl) + ' ml' : '' }), el('td', { class: cls, text: sym, title: r.status })));
  }
  $('#dProgBar').style.width = (D.status === 'COMPLETE' ? 100 : (D.steps ? done / D.steps * 100 : 0)) + '%';
  const s = $('#dProgStatus');
  s.textContent = D.status === 'COMPLETE' ? '✓ Complete' : D.status === 'ABORTED' ? '✗ Aborted' + (D.reason ? ': ' + D.reason : '') : 'Step ' + Math.min(done + 1, D.steps) + ' of ' + D.steps;
  s.className = D.status === 'COMPLETE' ? 'st-ok' : D.status === 'ABORTED' ? 'st-err' : 'st-run';
}

// Controller LEDs (§10.2)
function ledLabel(i) {
  if (i === 7) return 'Status';
  if (i === 8) return 'Wi-Fi';
  const b = S.config && S.config.buttons[i];
  if (!b) return 'Button ' + (i + 1);
  if (b.action === 'setRecipe') { const r = S.config.recipes.find((x) => x.id === b.recipeId); return r ? r.name : b.recipeId; }
  if (b.action === 'setVolume') return b.volumeMl + ' ml';
  return { dispense: 'Dispense', estop: 'E-Stop', none: '—' }[b.action] || b.action;
}
function renderLeds() {
  const box = $('#ledMirror');
  const leds = (S.state && S.state.leds) || [];
  if (box.children.length !== 9) { box.innerHTML = ''; for (let i = 0; i < 9; i++) box.append(el('div', { class: 'led off' }, el('i'), el('span'))); }
  for (let i = 0; i < 9; i++) {
    const L = leds[i] || { r: 0, g: 0, b: 0, mode: 'off' };
    const d = box.children[i];
    const mx = Math.max(L.r, L.g, L.b);
    const k = mx > 0 ? 255 / mx : 0; // controller LEDs are brightness-scaled; show the hue at full strength
    const mode = mx === 0 ? 'off' : (L.mode || 'solid');
    d.className = 'led ' + mode;
    d.style.setProperty('--c', 'rgb(' + Math.round(L.r * k) + ',' + Math.round(L.g * k) + ',' + Math.round(L.b * k) + ')');
    d.lastChild.textContent = (i < 7 ? (i + 1) + ': ' : '') + ledLabel(i);
    d.title = 'LED ' + (i + 1) + ' — ' + mode;
  }
}

// Prime All / Purge All (§9.6.2)
function bulkPlan(op) {
  const mult = op === 'purgeAll' ? 1.5 : 1;
  const q = (S.config ? S.config.pumps : []).filter((p) => isNum(p.autoPrimeSec) && p.autoPrimeSec > 0).sort((a, b) => a.pumpId - b.pumpId);
  const items = q.map((p) => {
    const rt = pumpRt(p.pumpId);
    const skip = !rt || !rt.present ? 'missing' : fwOld(rt.fwVersion) ? 'firmware too old' : '';
    return { pumpId: p.pumpId, liquid: p.liquid, ms: Math.round(mult * p.autoPrimeSec * 1000), skip };
  });
  const run = items.filter((x) => !x.skip);
  const est = run.reduce((s, x) => s + x.ms, 0) + Math.max(0, run.length - 1) * BULK_GAP_MS + run.length * 300;
  return { op, items, run, est };
}
function renderBulkBar() {
  const p = bulkPlan('primeAll');
  const ok = canDispense() && p.run.length > 0;
  setOff($('#bPrimeAll'), !ok);
  setOff($('#bPurgeAll'), !ok);
  $('#bulkHint').textContent = p.items.length ? p.run.length + ' pump' + (p.run.length === 1 ? '' : 's') + ' with auto prime' + (p.items.length > p.run.length ? ', ' + (p.items.length - p.run.length) + ' will be skipped' : '') : 'No pumps have an auto prime time set';
}
async function startBulk(op) {
  const p = bulkPlan(op);
  const body = el('div', null,
    el('p', { class: 'note', text: op === 'primeAll' ? 'Put a container under every outlet before you start.' : 'Purge runs the pumps in reverse and pushes the liquid in the lines back into the bottles.' }),
    el('table', { class: 'tbl compact' }, el('thead', null, el('tr', null, el('th', { text: 'Pump' }), el('th', { text: 'Liquid' }), el('th', { text: 'Seconds' }))),
      el('tbody', null, p.items.map((x) => el('tr', { class: x.skip ? 'muted' : '' }, el('td', { text: x.pumpId }), el('td', { text: x.liquid || '(unloaded)' }),
        el('td', { text: x.skip ? 'skipped — ' + x.skip : (x.ms / 1000).toFixed(1) + ' s' }))))),
    el('p', null, 'Estimated total time: ', el('strong', { text: (p.est / 1000).toFixed(1) + ' s' })));
  if (await confirmDlg(op === 'primeAll' ? 'Prime All' : 'Purge All', body, op === 'primeAll' ? 'Start Prime All' : 'Start Purge All', op === 'purgeAll')) cmd(op);
}
$('#bPrimeAll').addEventListener('click', () => startBulk('primeAll'));
$('#bPurgeAll').addEventListener('click', () => startBulk('purgeAll'));

function onBulkProgress(m) {
  if (m.op === 'prime' || m.op === 'purge') { onManualProgress(m); return; }
  let B = S.bulk;
  if (m.status === 'STARTING' || !B || B.op !== m.op || B.final) {
    const plan = bulkPlan(m.op);
    B = S.bulk = { op: m.op, steps: m.steps || plan.items.length, est: m.estimatedTotalMs || plan.est, rows: {}, final: false };
    plan.items.forEach((x, i) => { B.rows[i + 1] = { pumpId: x.pumpId, liquid: x.liquid, ms: x.ms, status: 'PENDING' }; });
  }
  if (m.steps) B.steps = m.steps;
  if (m.status === 'COMPLETE' || m.status === 'ABORTED') {
    B.final = true; B.status = m.status; B.reason = m.reason;
    if (m.status === 'ABORTED' && B.rows[m.step] && B.rows[m.step].status === 'RUNNING') B.rows[m.step].status = 'ABORTED';
  } else if (m.step > 0) {
    const r = B.rows[m.step] || (B.rows[m.step] = {});
    if (m.pumpId != null) r.pumpId = m.pumpId;
    if (m.liquid != null) r.liquid = m.liquid;
    if (isNum(m.durationMs)) r.ms = m.durationMs;
    r.status = m.status;
  }
  renderBulkProgress();
  renderPumpCards();
}
function renderBulkProgress() {
  const B = S.bulk;
  $('#bulkProgress').hidden = !B;
  if (!B) return;
  $('#bulkTitle').textContent = (B.op === 'primeAll' ? 'Prime All' : 'Purge All') + ' — about ' + (B.est / 1000).toFixed(1) + ' s';
  const body = $('#bulkRows');
  body.innerHTML = '';
  let done = 0;
  for (let i = 1; i <= B.steps; i++) {
    const r = B.rows[i] || { status: 'PENDING' };
    const map = { DONE: ['✓', 'st-ok'], SKIPPED: ['skipped', 'muted'], RUNNING: ['…', 'st-run'], ABORTED: ['✗', 'st-err'], PENDING: ['', 'muted'] };
    const [sym, cls] = map[r.status] || ['', ''];
    if (r.status === 'DONE' || r.status === 'SKIPPED') done++;
    body.append(el('tr', null, el('td', { text: r.pumpId != null ? r.pumpId : '' }), el('td', { text: r.liquid || '' }),
      el('td', { text: isNum(r.ms) ? (r.ms / 1000).toFixed(1) + ' s' : '' }), el('td', { class: cls, text: sym })));
  }
  $('#bulkBar').style.width = (B.status === 'COMPLETE' ? 100 : (B.steps ? done / B.steps * 100 : 0)) + '%';
  const s = $('#bulkStatus');
  s.textContent = B.status === 'COMPLETE' ? '✓ Complete' : B.status === 'ABORTED' ? '✗ Aborted' + (B.reason ? ': ' + B.reason : '') : done + ' of ' + B.steps + ' done';
  s.className = B.status === 'COMPLETE' ? 'st-ok' : B.status === 'ABORTED' ? 'st-err' : 'st-run';
}

// Pump cards
const cardCache = {};
function makeCard(id) {
  const c = { root: el('div', { class: 'card pcard' }) };
  c.id = el('strong'); c.bridge = el('span', { class: 'badge bridge', text: 'bridge' }); c.badge = el('span', { class: 'badge' });
  c.liq = el('div', { class: 'liq' });
  c.kv = el('dl', { class: 'kv' });
  c.lvl = el('div', { class: 'lvl' }, el('div'));
  c.strip = el('div', { class: 'strip', title: 'Pump LED strip (tracked state)' });
  for (let i = 0; i < 20; i++) c.strip.append(el('i'));
  c.auto = el('div', { class: 'small muted' });
  c.pBtn = el('button', { class: 'btn small', 'data-needs-conn': '', onclick: () => single('prime', id) });
  c.uBtn = el('button', { class: 'btn small warn', 'data-needs-conn': '', onclick: () => single('purge', id) });
  c.run = el('span', { class: 'muted small' });
  c.root.append(el('div', { class: 'top' }, el('span', null, c.id, ' ', c.bridge), c.badge), c.liq, c.lvl, c.kv, c.strip, c.auto,
    el('div', { class: 'acts' }, c.pBtn, c.uBtn, c.run));
  return c;
}

// Manual prime/purge (§9.6.1): runs until stopped, at most MANUAL_MAX_MS; one pump at a time.
// Pressing Prime/Purge on another pump makes the controller stop the running one first.
const MANUAL_MAX_MS = 120000;
function onManualProgress(m) {
  if (m.status === 'RUNNING') S.manual = { op: m.op, pumpId: m.pumpId, since: Date.now() };
  else if (m.status === 'DONE' || m.status === 'ABORTED' || m.status === 'COMPLETE') {
    if (S.manual && (m.pumpId == null || S.manual.pumpId === m.pumpId)) S.manual = null;
  }
  if (S.manualReq && (m.status === 'RUNNING' || m.status === 'ABORTED')) S.manualReq = null;
  renderPumpCards();
}
// The pump running a manual prime/purge, from bulkProgress or (after a reconnect) the pump activity
function manualRun() {
  if (S.manual) return S.manual;
  if (sys() !== 'MANUAL' || (S.bulk && !S.bulk.final)) return null;
  const rt = (S.pumps || []).find((p) => p.activity === 'PRIMING' || p.activity === 'PURGING');
  return rt ? { op: rt.activity === 'PRIMING' ? 'prime' : 'purge', pumpId: rt.pumpId, since: null } : null;
}
function single(kind, id) {
  const run = manualRun();
  if (run && run.pumpId === id && run.op === kind) { cmd('pumpStop', { pumpId: id }); return; }
  S.manualReq = { op: kind, pumpId: id };
  renderPumpCards();
  cmd(kind, { pumpId: id }).then((d) => { if (d === undefined) { S.manualReq = null; renderPumpCards(); } });
}
setInterval(() => { if (S.tab === 'dash' && S.manual) renderPumpCards(); }, 1000);
function renderPumpCards() {
  if (!S.config) return;
  const box = $('#pumpCards');
  const cfg = S.config;
  const ids = cfg.pumps.map((p) => p.pumpId);
  if (box.dataset.ids !== ids.join(',')) {
    box.innerHTML = '';
    ids.forEach((id) => { if (!cardCache[id]) cardCache[id] = makeCard(id); box.append(cardCache[id].root); });
    box.dataset.ids = ids.join(',');
    if (!ids.length) box.append(el('p', { class: 'muted', text: 'No pumps configured. Add them on the Pumps tab.' }));
  }
  const L = cfg.levels;
  const bulkRun = S.bulk && !S.bulk.final ? Object.values(S.bulk.rows).find((r) => r.status === 'RUNNING') : null;
  cfg.pumps.forEach((p) => {
    const c = cardCache[p.pumpId], rt = pumpRt(p.pumpId) || {};
    c.id.textContent = 'Pump ' + p.pumpId;
    c.bridge.hidden = !rt.isBridge;
    c.liq.textContent = p.liquid || '(unloaded)';
    c.liq.style.borderLeft = '4px solid ' + rgbHex(p.color);
    c.liq.style.paddingLeft = '6px';
    const ls = rt.present === false ? 'UNKNOWN' : (rt.levelState || 'UNKNOWN');
    c.badge.textContent = ls; c.badge.className = 'badge ' + ls;
    const status = rt.present === false ? 'Missing' : rt.present === undefined ? '—' : fwOld(rt.fwVersion) ? 'FW too old' : rt.activity && rt.activity !== 'IDLE' ? 'Busy (' + rt.activity.toLowerCase() + ')' : 'Present';
    c.kv.innerHTML = '';
    [['Status', status], ['Firmware', rt.fwVersion || '—'], ['Weight', isNum(rt.weightG) ? fmt1(rt.weightG) + ' g' : '—'], ['Level', isNum(rt.levelMl) ? fmt1(rt.levelMl) + ' ml' : '—']]
      .forEach(([k, v]) => c.kv.append(el('dt', { text: k }), el('dd', { text: v, class: k === 'Status' && status !== 'Present' && status !== '—' ? (status === 'Missing' || status === 'FW too old' ? 'st-err' : 'st-run') : null })));
    const max = Math.max(750, L.lowMl * 2, isNum(rt.levelMl) ? rt.levelMl : 0);
    const bar = c.lvl.firstChild;
    bar.style.width = (isNum(rt.levelMl) ? Math.min(100, rt.levelMl / max * 100) : 0) + '%';
    bar.className = ls;
    while (c.lvl.children.length > 1) c.lvl.lastChild.remove();
    [['lowMl', 'Low'], ['emptyMl', 'Indicated empty'], ['actualEmptyMl', 'Actually empty']].forEach(([k, t]) =>
      c.lvl.append(el('b', { style: 'left:' + (L[k] / max * 100) + '%', title: t + ': ' + L[k] + ' ml' })));
    c.lvl.title = isNum(rt.levelMl) ? fmt1(rt.levelMl) + ' ml' : 'unknown';
    c.led = rt.present === false ? null : rt.led;
    c.auto.textContent = p.autoPrimeSec > 0 ? 'Auto prime: ' + p.autoPrimeSec.toFixed(1) + ' s / purge ' + (p.autoPrimeSec * 1.5).toFixed(1) + ' s' : 'Auto prime: off';
    const running = (rt.activity && rt.activity !== 'IDLE') || (bulkRun && bulkRun.pumpId === p.pumpId);
    c.root.classList.toggle('running', !!running);
    c.root.classList.toggle('missing', rt.present === false);
    // A manual run may be replaced by another, so the buttons stay live during one
    const run = manualRun(), req = S.manualReq;
    const canStart = S.connected && !errorsOf('blocking').length && (!isBusy() || !!run);
    const off = !canStart || rt.present === false || fwOld(rt.fwVersion);
    [['prime', c.pBtn, 'Prime', 'btn small'], ['purge', c.uBtn, 'Purge', 'btn small warn']].forEach(([op, b, label, cls]) => {
      const mine = run && run.pumpId === p.pumpId && run.op === op;
      const pending = !mine && req && req.pumpId === p.pumpId && req.op === op;
      b.textContent = mine ? '■ Stop ' + label.toLowerCase() : pending ? label + '…' : label;
      b.className = mine ? 'btn small danger' : cls;
      b.title = mine ? 'Stop pump ' + p.pumpId : label + ' pump ' + p.pumpId + ' until you press Stop (at most ' + MANUAL_MAX_MS / 1000 + ' s)' + (run && run.pumpId !== p.pumpId ? '; stops pump ' + run.pumpId + ' first' : '');
      setOff(b, mine ? !S.connected : off);
    });
    const mineRun = run && run.pumpId === p.pumpId;
    c.run.textContent = mineRun && run.since ? Math.min(MANUAL_MAX_MS / 1000, Math.floor((Date.now() - run.since) / 1000)) + ' / ' + MANUAL_MAX_MS / 1000 + ' s' : '';
  });
}
// Pump LED strip preview: 20 dots, modes 1 solid, 2 fade, 3 loop chase, 4 bounce (50 ms steps, tail 4).
setInterval(() => {
  if (S.tab !== 'dash') return;
  const now = Date.now();
  for (const id in cardCache) {
    const c = cardCache[id];
    if (!c.root.isConnected) continue;
    const L = c.led, dots = c.strip.children;
    for (let i = 0; i < 20; i++) {
      let k = 0;
      if (L && L.brightness > 0) {
        const br = L.brightness / 255;
        if (L.mode === 1) k = br;
        else if (L.mode === 2) { const t = now % 4000; k = br * (t < 1500 ? t / 1500 : t < 2000 ? 1 : t < 3500 ? 1 - (t - 2000) / 1500 : 0); }
        else if (L.mode === 3 || L.mode === 4) {
          const step = Math.floor(now / 50);
          let head = L.mode === 3 ? step % 20 : (step % 38 < 19 ? step % 38 : 38 - step % 38);
          const d = Math.abs(i - head);
          k = d <= 4 ? br * (1 - d / 5) : 0;
        }
      }
      dots[i].style.background = k > 0.02 ? 'rgba(' + L.r + ',' + L.g + ',' + L.b + ',' + (0.15 + 0.85 * k).toFixed(2) + ')' : '';
    }
  }
}, 50);

// ------------------------------------------------------------------ Recipes editor (§15.4)
let rSel = 0;
let rNew = new WeakSet(), rIdTouched = new WeakSet();
defEditor('recipes', ['recipes'], (ed) => {
  if (!ed.dirty) { rNew = new WeakSet(); rIdTouched = new WeakSet(); }
  const list = $('#rList');
  const recs = ed.draft.recipes;
  if (rSel >= recs.length) rSel = recs.length - 1;
  if (rSel < 0) rSel = 0;
  list.innerHTML = '';
  const bad = new Set(ed.errors.map((e) => (/^recipes\[(\d+)\]/.exec(e.path) || [])[1]).filter((x) => x != null).map(Number));
  recs.forEach((r, i) => {
    const prob = recipeProblem(r, activeVolume());
    list.append(el('li', { class: i === rSel ? 'sel' : '', onclick: () => { rSel = i; ed.render(ed); showErrors(ed); } },
      el('span', { text: r.name || '(unnamed)', class: bad.has(i) ? 'st-err' : '' }),
      prob ? el('span', { class: 'small muted', text: 'unavailable', title: prob }) : null));
  });
  if (!recs.length) list.append(el('li', { class: 'muted', text: 'No recipes yet' }));
  renderRecipeEditor(ed);
});
function renderRecipeEditor(ed) {
  const box = $('#rEditor');
  box.innerHTML = '';
  const r = ed.draft.recipes[rSel];
  if (!r) { box.append(el('p', { class: 'muted', text: 'Add a recipe to get started.' })); return; }
  const P = 'recipes[' + rSel + ']';
  const isNew = rNew.has(r);
  const liquids = (S.config ? S.config.pumps : []).map((p) => p.liquid).filter(Boolean);
  const dl = el('datalist', { id: 'rLiquids' }, liquids.map((l) => el('option', { value: l })));
  const tbody = el('tbody');
  r.ingredients.forEach((g, j) => {
    const G = P + '.ingredients[' + j + ']';
    const known = liquids.some((l) => l.toLowerCase() === String(g.liquid || '').trim().toLowerCase());
    tbody.append(el('tr', { dataset: { idx: j } },
      el('td', null, el('span', { class: 'handle', title: 'Drag to reorder', text: '⠿' })),
      el('td', null, inp(G + '.liquid', g.liquid, { list: 'rLiquids', placeholder: 'liquid', maxlength: 32 }),
        g.liquid && !known ? el('span', { class: 'small st-warn', text: ' not loaded' }) : null),
      el('td', null, inp(G + '.parts', g.parts, { type: 'number', min: 0, step: 0.01 })),
      el('td', null, inp(G + '.order', g.order, { type: 'number', min: 0, step: 1, dataset: { type: 'int' } })),
      el('td', null, el('button', { class: 'btn small danger', text: '✕', title: 'Remove', onclick: () => { r.ingredients.splice(j, 1); markDirty(ed); renderRecipeEditor(ed); } }))));
  });
  box.append(dl,
    el('div', { class: 'row wrap' },
      el('label', { class: 'fld grow' }, 'Name', inp(P + '.name', r.name, { maxlength: 32 })),
      el('label', { class: 'fld' }, 'ID' + (isNew ? '' : ' (fixed after first save)'), inp(P + '.id', r.id, { maxlength: 32, readOnly: !isNew, pattern: '[a-z0-9-]{1,32}' }))),
    el('h3', { style: 'margin-top:12px', text: 'Ingredients' }),
    el('div', { class: 'tblwrap' }, el('table', { class: 'tbl', id: 'rIngTable' },
      el('thead', null, el('tr', null, el('th'), el('th', { text: 'Liquid' }), el('th', { text: 'Parts' }), el('th', { text: 'Order' }), el('th'))), tbody)),
    el('button', { class: 'btn small', style: 'margin-top:6px', text: '+ Ingredient', onclick: () => {
      const mo = r.ingredients.reduce((m, g) => Math.max(m, isNum(g.order) ? g.order : 0), 0);
      r.ingredients.push({ liquid: '', parts: 1, order: mo + 1 }); markDirty(ed); renderRecipeEditor(ed);
    } }),
    el('div', { class: 'preview', id: 'rPreview' }));
  updatePreview(ed);
  enableDrag(tbody, ed, r);
}
function updatePreview(ed) {
  const r = ed.draft.recipes[rSel], pv = $('#rPreview');
  if (!r || !pv) return;
  const v = activeVolume();
  const steps = recipeSteps(r, v).filter((s) => s.liquid);
  pv.textContent = steps.length ? 'At ' + v + ' ml: ' + steps.map((s) => s.liquid + ' ' + fmt1(s.ml) + ' ml').join(', ') : '';
}
// Pointer-based drag reorder (works for mouse and touch); renumbers `order`.
function enableDrag(tbody, ed, r) {
  let drag = null;
  tbody.addEventListener('pointerdown', (e) => {
    const h = e.target.closest('.handle');
    if (!h) return;
    drag = h.closest('tr'); drag.classList.add('dragging');
    h.setPointerCapture(e.pointerId); e.preventDefault();
  });
  tbody.addEventListener('pointermove', (e) => {
    if (!drag) return;
    const rows = $$('tr', tbody);
    for (const row of rows) {
      if (row === drag) continue;
      const b = row.getBoundingClientRect();
      if (e.clientY > b.top && e.clientY < b.bottom) {
        if (e.clientY < b.top + b.height / 2) tbody.insertBefore(drag, row); else tbody.insertBefore(drag, row.nextSibling);
        break;
      }
    }
  });
  const end = () => {
    if (!drag) return;
    drag.classList.remove('dragging');
    const order = $$('tr', tbody).map((row) => +row.dataset.idx);
    drag = null;
    if (order.some((v, i) => v !== i)) {
      r.ingredients = order.map((i) => r.ingredients[i]);
      r.ingredients.forEach((g, i) => { g.order = i + 1; });
      markDirty(ed);
    }
    renderRecipeEditor(ed);
  };
  tbody.addEventListener('pointerup', end);
  tbody.addEventListener('pointercancel', end);
}
(function () {
  const ed = editors.recipes;
  bindEditor(ed);
  ed.onField = (ed, f) => {
    const r = ed.draft.recipes[rSel];
    if (!r) return;
    if (/\.id$/.test(f.dataset.path)) rIdTouched.add(r);
    if (/\.name$/.test(f.dataset.path)) {
      if (rNew.has(r) && !rIdTouched.has(r)) { r.id = uniqueId(slugify(r.name), r); const idf = $$('[data-path]', ed.sec).find((x) => x.dataset.path === 'recipes[' + rSel + '].id'); if (idf) idf.value = r.id; }
      const li = $('#rList').children[rSel]; if (li) li.firstChild.textContent = r.name || '(unnamed)';
    }
    updatePreview(ed);
  };
  ed.onCommit = (ed, t) => { if (t.closest && t.closest('[data-path*="ingredients"]')) renderRecipeEditor(ed); };
  function uniqueId(base, self) {
    const used = new Set(ed.draft.recipes.filter((x) => x !== self).map((x) => x.id));
    let id = base.slice(0, 32), n = 2;
    while (used.has(id)) { const suf = '-' + n++; id = base.slice(0, 32 - suf.length) + suf; }
    return id;
  }
  $('#rAdd').addEventListener('click', () => {
    const r = { id: '', name: 'New recipe', ingredients: [{ liquid: '', parts: 1, order: 1 }] };
    r.id = uniqueId('new-recipe', r);
    ed.draft.recipes.push(r); rNew.add(r); rSel = ed.draft.recipes.length - 1; markDirty(ed); ed.render(ed);
  });
  $('#rDup').addEventListener('click', () => {
    const src = ed.draft.recipes[rSel];
    if (!src) return;
    const r = clone(src);
    r.name = (src.name + ' copy').slice(0, 32);
    r.id = uniqueId(slugify(r.name), r);
    ed.draft.recipes.splice(rSel + 1, 0, r); rNew.add(r); rSel++; markDirty(ed); ed.render(ed);
  });
  $('#rDel').addEventListener('click', async () => {
    const r = ed.draft.recipes[rSel];
    if (!r) return;
    const refs = (S.config.buttons || []).map((b, i) => (b.action === 'setRecipe' && b.recipeId === r.id ? i + 1 : 0)).filter(Boolean);
    const msg = 'Delete “' + r.name + '”?' + (refs.length ? ' Button' + (refs.length > 1 ? 's ' : ' ') + refs.join(', ') + ' use it; change them on the Buttons tab or the save will be rejected.' : '');
    if (!await confirmDlg('Delete recipe', msg, 'Delete', true)) return;
    ed.draft.recipes.splice(rSel, 1); markDirty(ed); ed.render(ed);
  });
})();

// ------------------------------------------------------------------ Pumps editor (§15.5)
defEditor('pumps', ['pumps'], (ed) => {
  const tb = $('#pTable tbody');
  tb.innerHTML = '';
  ed.draft.pumps.forEach((p, i) => {
    const P = 'pumps[' + i + ']';
    tb.append(el('tr', null,
      el('td', null, inp(P + '.pumpId', p.pumpId, { type: 'number', min: 1, max: 254, step: 1, style: 'width:70px', dataset: { type: 'int' } })),
      el('td', null, inp(P + '.liquid', p.liquid, { maxlength: 32, placeholder: '(unloaded)' })),
      el('td', null, inp(P + '.msPerMl', p.msPerMl, { type: 'number', min: 1, max: 65535, step: 1, dataset: { type: 'int' } })),
      el('td', null, inp(P + '.densityGPerMl', p.densityGPerMl, { type: 'number', min: 0.5, max: 2, step: 0.01, style: 'width:76px' })),
      el('td', null, inp(P + '.color', p.color, { dataset: { type: 'color' } })),
      el('td', null, el('div', { class: 'row', style: 'align-items:center' }, inp(P + '.autoPrimeSec', p.autoPrimeSec, { type: 'number', min: 0, max: 60, step: 0.1, style: 'width:70px' }),
        el('span', { class: 'small muted', dataset: { purge: i }, text: purgeText(p.autoPrimeSec) }))),
      el('td', { class: 'small', dataset: { status: i } }),
      el('td', null, el('button', { class: 'btn small danger', text: '✕', title: 'Remove pump', onclick: () => { ed.draft.pumps.splice(i, 1); markDirty(ed); ed.render(ed); } }))));
  });
  if (!ed.draft.pumps.length) tb.append(el('tr', null, el('td', { colspan: 8, class: 'muted', text: 'No pumps configured.' })));
  updatePumpStatusCells();
});
function purgeText(a) { return isNum(a) && a > 0 ? 'purge = 1.5× = ' + (a * 1.5).toFixed(1) + ' s' : 'off'; }
function updatePumpStatusCells() {
  const ed = editors.pumps;
  if (!ed || !ed.draft) return;
  $$('[data-status]', ed.sec).forEach((td) => {
    const p = ed.draft.pumps[+td.dataset.status];
    const rt = p && pumpRt(p.pumpId);
    if (!rt) { td.textContent = 'not found'; td.className = 'small muted'; return; }
    td.className = 'small ' + (!rt.present ? 'st-err' : rt.levelState === 'OK' ? 'st-ok' : rt.levelState === 'UNKNOWN' ? 'muted' : 'st-warn');
    td.textContent = !rt.present ? 'Missing' : (fwOld(rt.fwVersion) ? 'FW too old · ' : '') + (rt.activity !== 'IDLE' ? rt.activity.toLowerCase() + ' · ' : '') + rt.levelState + ' ' + fmt1(rt.levelMl) + ' ml';
  });
}
(function () {
  const ed = editors.pumps;
  bindEditor(ed);
  ed.onField = (ed, f) => {
    const m = /^pumps\[(\d+)\]\.autoPrimeSec$/.exec(f.dataset.path);
    if (m) { const s = $('[data-purge="' + m[1] + '"]', ed.sec); if (s) s.textContent = purgeText(ed.draft.pumps[+m[1]].autoPrimeSec); }
  };
  $('#pAdd').addEventListener('click', () => {
    const used = new Set(ed.draft.pumps.map((p) => p.pumpId));
    let id = 1; while (used.has(id) && id < 254) id++;
    ed.draft.pumps.push({ pumpId: id, liquid: '', msPerMl: 100, densityGPerMl: 1.0, color: [255, 255, 255], autoPrimeSec: S.config ? S.config.defaults.primeSec : 0 });
    markDirty(ed); ed.render(ed);
  });
})();

// ------------------------------------------------------------------ Buttons editor (§15.6)
const ACTIONS = [['none', 'None'], ['setRecipe', 'Set recipe'], ['setVolume', 'Set volume'], ['dispense', 'Dispense'], ['estop', 'E-Stop']];
defEditor('buttons', ['buttons'], (ed) => {
  while (ed.draft.buttons.length < 7) ed.draft.buttons.push({ action: 'none' });
  const tb = $('#bTable tbody');
  tb.innerHTML = '';
  ed.draft.buttons.forEach((b, i) => {
    const B = 'buttons[' + i + ']';
    let param = el('span', { class: 'muted small', text: b.action === 'estop' ? 'Hold 3 s to clear latched errors' : '—' });
    if (b.action === 'setRecipe') {
      param = el('select', { dataset: { path: B + '.recipeId' } }, el('option', { value: '', text: '— choose —' }),
        (S.config ? S.config.recipes : []).map((r) => el('option', { value: r.id, text: r.name })));
      param.value = b.recipeId || '';
    } else if (b.action === 'setVolume') {
      param = el('span', null, inp(B + '.volumeMl', b.volumeMl, { type: 'number', step: 1, min: S.config.dispense.minVolumeMl, max: S.config.dispense.maxVolumeMl }), ' ml');
    }
    const act = el('select', { dataset: { path: B + '.action' } }, ACTIONS.map(([v, t]) => el('option', { value: v, text: t })));
    act.value = b.action;
    tb.append(el('tr', null, el('td', { text: 'Button ' + (i + 1) }), el('td', null, act), el('td', null, param)));
  });
});
(function () {
  const ed = editors.buttons;
  bindEditor(ed);
  ed.onField = (ed, f) => {
    const m = /^buttons\[(\d+)\]\.action$/.exec(f.dataset.path);
    if (!m) return;
    const i = +m[1], a = ed.draft.buttons[i].action, nb = { action: a };
    if (a === 'setRecipe') nb.recipeId = S.config.recipes.length ? S.config.recipes[0].id : '';
    if (a === 'setVolume') nb.volumeMl = S.config.defaults.volumeMl;
    ed.draft.buttons[i] = nb;
    ed.render(ed);
  };
})();

// ------------------------------------------------------------------ Wi-Fi editor (§15.6)
defEditor('wifi', ['wifi'], (ed) => {
  const nets = ed.draft.wifi.networks || (ed.draft.wifi.networks = []);
  const tb = $('#wTable tbody');
  tb.innerHTML = '';
  const move = (i, d) => { const j = i + d; if (j < 0 || j >= nets.length) return; const t = nets[i]; nets[i] = nets[j]; nets[j] = t; markDirty(ed); ed.render(ed); };
  nets.forEach((n, i) => {
    const W = 'wifi.networks[' + i + ']';
    const pw = inp(W + '.password', n.password, { type: 'password', autocomplete: 'off', placeholder: '(open network)', maxlength: 63 });
    const eye = el('button', { class: 'btn small', text: 'Show', type: 'button', onclick: () => { const show = pw.type === 'password'; pw.type = show ? 'text' : 'password'; eye.textContent = show ? 'Hide' : 'Show'; } });
    tb.append(el('tr', null, el('td', { text: i + 1 }),
      el('td', null, inp(W + '.ssid', n.ssid, { maxlength: 32, autocomplete: 'off' })),
      el('td', null, el('div', { class: 'row', style: 'align-items:center' }, pw, eye)),
      el('td', null, el('div', { class: 'row' },
        el('button', { class: 'btn small', text: '↑', title: 'Move up', disabled: i === 0, onclick: () => move(i, -1) }),
        el('button', { class: 'btn small', text: '↓', title: 'Move down', disabled: i === nets.length - 1, onclick: () => move(i, 1) }),
        el('button', { class: 'btn small danger', text: '✕', title: 'Remove', onclick: () => { nets.splice(i, 1); markDirty(ed); ed.render(ed); } })))));
  });
  if (!nets.length) tb.append(el('tr', null, el('td', { colspan: 4, class: 'muted', text: 'No networks: the controller will start its own access point.' })));
});
(function () {
  const ed = editors.wifi;
  bindEditor(ed);
  $('#wAdd').addEventListener('click', () => {
    const nets = ed.draft.wifi.networks;
    if (nets.length >= 8) { toast('At most 8 networks', 'err'); return; }
    nets.push({ ssid: '', password: '' }); markDirty(ed); ed.render(ed);
  });
})();

// ------------------------------------------------------------------ Settings (§15.6)
const SETTINGS = [
  ['Liquid levels', [
    ['levels.lowMl', 'Low (ml)', { min: 0, step: 1 }, 'Pump LEDs pulse yellow at or below this'],
    ['levels.emptyMl', 'Indicated empty (ml)', { min: 0, step: 1 }, 'Pump LEDs pulse red at or below this'],
    ['levels.actualEmptyMl', 'Actually empty (ml)', { min: 0, step: 1 }, 'Unpumpable reserve; never dispensed below']]],
  ['Dispensing', [
    ['dispense.verifyTolerancePct', 'Verify tolerance (%)', { min: 0, max: 100, step: 1 }],
    ['dispense.verifyToleranceMinMl', 'Verify tolerance min (ml)', { min: 0, step: 0.5 }],
    ['dispense.settleMs', 'Settle time (ms)', { min: 0, max: 10000, step: 100, int: 1 }],
    ['dispense.minVolumeMl', 'Min drink volume (ml)', { min: 1, max: 1000, step: 1 }],
    ['dispense.maxVolumeMl', 'Max drink volume (ml)', { min: 1, max: 1000, step: 1 }]]],
  ['Defaults at boot', [
    ['defaults.volumeMl', 'Drink volume (ml)', { min: 1, max: 1000, step: 1 }],
    ['defaults.recipeId', 'Recipe', { select: 'recipes' }],
    ['defaults.primeSec', 'Auto prime for a new pump (s)', { min: 1, max: 60, step: 1 }]]],
  ['Device', [
    ['ui.ledBrightness', 'Controller LED brightness (0–255)', { min: 0, max: 255, step: 1, int: 1 }],
    ['device.hostname', 'Hostname (applies after reboot)', { text: 1 }, '→ http://<hostname>.local'],
    ['device.apSsid', 'Access point SSID (applies after reboot)', { text: 1 }]]],
];
defEditor('settings', ['levels', 'dispense', 'defaults', 'ui', 'device'], (ed) => {
  const f = $('#sForm');
  f.innerHTML = '';
  for (const [title, fields] of SETTINGS) {
    const card = el('div', { class: 'card' }, el('h3', { text: title }));
    for (const [path, label, o, hint] of fields) {
      const v = getPath(ed.draft, path);
      let input;
      if (o.select === 'recipes') {
        input = el('select', { dataset: { path } }, el('option', { value: '', text: '— none —' }), (S.config ? S.config.recipes : []).map((r) => el('option', { value: r.id, text: r.name })));
        input.value = v || '';
      } else if (o.text) input = inp(path, v, { maxlength: 32 });
      else input = inp(path, v, { type: 'number', min: o.min, max: o.max, step: o.step, dataset: o.int ? { type: 'int' } : {} });
      card.append(el('label', { class: 'fld', style: 'margin-bottom:8px' }, label, input, hint ? el('span', { class: 'small muted', text: hint }) : null));
    }
    f.append(card);
  }
});
bindEditor(editors.settings);

// Import / export
$('#sImport').addEventListener('change', async (e) => {
  const file = e.target.files[0];
  e.target.value = '';
  const msg = $('#sImportMsg');
  msg.innerHTML = '';
  if (!file) return;
  let cfg;
  try { cfg = JSON.parse(await file.text()); } catch (err) { msg.append(el('span', { class: 'st-err', text: 'Not valid JSON: ' + err.message })); return; }
  if (file.size > 32768) { msg.append(el('span', { class: 'st-err', text: 'File is larger than 32 KB' })); return; }
  const errs = validateConfig(trimAll(cfg));
  if (errs.length) { showImportErrors(errs); return; }
  const ok = await confirmDlg('Import config?', 'This replaces the whole configuration (' + (cfg.pumps || []).length + ' pumps, ' + (cfg.recipes || []).length + ' recipes, ' +
    ((cfg.wifi && cfg.wifi.networks) || []).length + ' networks). Wi-Fi and hostname changes apply after reboot.', 'Import', true);
  if (!ok) return;
  try {
    const r = await fetch('/api/config', { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(cfg) });
    let body = {};
    try { body = await r.json(); } catch (x) { /* empty body */ }
    if (r.ok && body.ok !== false) { msg.append(el('span', { class: 'st-ok', text: '✓ Imported ' + file.name })); toast('Config imported', 'ok'); }
    else if (r.status === 409) msg.append(el('span', { class: 'st-err', text: 'Rejected: the controller is busy. Try again when idle.' }));
    else showImportErrors(normErrors(body.errors || body.details || [{ path: '', message: 'HTTP ' + r.status }]));
  } catch (err) { msg.append(el('span', { class: 'st-err', text: 'Upload failed: ' + err.message })); }
});
function showImportErrors(errs) {
  $('#sImportMsg').append(el('div', { class: 'errsum' }, el('strong', { text: 'Import rejected (' + errs.length + '):' }),
    el('ul', null, errs.map((e) => el('li', null, el('code', { text: e.path || '(config)' }), ' ', e.message)))));
}

// Pump firmware: uploading stores a file; only the Update pumps button ever touches a pump
function boardName(id) { return id === 0 ? 'bridge' : 'pump ' + id; }
function boardList(ids) { return ids.map(boardName).join(', '); }
function renderPumpFw() {
  const m = S.pumpFw;
  if (!m) return;
  const u = m.update || {};
  const info = $('#pfInfo');
  info.replaceChildren();
  const row = (k, v) => { info.append(el('dt', { text: k }), el('dd', { text: v })); };
  row('Stored file', m.image ? m.image.version + ' (' + Math.round(m.image.size / 1024) + ' KB)' : 'none');
  if (m.bridgeId === 0) row('Bridge', m.bridgeVersion || '—');
  const targets = m.targets || [];
  const box = $('#pfTargets');
  box.replaceChildren();
  if (!m.image) box.append(el('div', { class: 'muted', text: 'Upload a pump firmware .bin (0.7.1 or newer) to check for updates.' }));
  else if (!targets.length && !u.active) box.append(el('div', { class: 'st-ok', text: 'All reachable boards have this firmware or newer.' }));
  if (targets.length && !u.active) {
    box.append(el('div', { class: 'st-warn', text: 'Update available for: ' + boardList(targets.map((t) => t.pumpId)) }));
    box.append(el('div', { class: 'muted', text: 'About ' + Math.ceil(targets.length * 0.85) + ' min. They update one at a time, bridge last. No drinks during the update.' }));
  }
  if (m.needsUsb && m.needsUsb.length) box.append(el('div', { class: 'st-err', text: 'Too old to update here, flash over USB: ' + boardList(m.needsUsb) }));
  if (m.missing && m.missing.length) box.append(el('div', { class: 'muted', text: 'Not answering, so left alone: ' + boardList(m.missing) }));

  $('#pfBarWrap').hidden = !u.active;
  const st = $('#pfStatus');
  if (u.active) {
    const pct = u.size ? Math.floor(u.bytes * 100 / u.size) : 0;
    $('#pfBar').style.width = (u.phase === 'INSTALL' ? 100 : pct) + '%';
    const name = boardName(u.pumpId);
    st.className = 'small st-run';
    st.textContent = name.charAt(0).toUpperCase() + name.slice(1) + ' (' + u.index + ' of ' + u.total + '): ' +
      (u.phase === 'INSTALL' ? 'installing and restarting…' : 'sending ' + pct + '%');
  } else if (u.result) {
    st.className = 'small ' + (u.result === 'COMPLETE' ? 'st-ok' : u.result === 'FAILED' ? 'st-err' : 'st-warn');
    st.textContent = (u.result === 'COMPLETE' ? '✓ Update complete: ' : u.result === 'FAILED' ? '✗ Update stopped: ' : 'Cancelled: ') + u.message;
  } else {
    st.textContent = '';
  }
  $('#pfUpdate').hidden = !!u.active;
  $('#pfCancel').hidden = !u.active;
  $('#pfDelete').hidden = !m.image || !!u.active;
  $('#pfUpdate').disabled = !S.connected || !targets.length || isBusy();
}
$('#pfUpload').addEventListener('click', () => {
  const file = $('#pfFile').files[0];
  const msg = $('#pfUploadMsg');
  if (!file) { toast('Choose a pump firmware .bin first', 'err'); return; }
  if (!/\.bin$/i.test(file.name)) { toast('Pump firmware must be a .bin file', 'err'); return; }
  if (isBusy()) { toast('Upload when the controller is idle', 'err'); return; }
  const fd = new FormData();
  fd.append('firmware', file, file.name);
  const xhr = new XMLHttpRequest();
  msg.className = 'small muted';
  msg.textContent = 'Uploading…';
  xhr.onload = () => {
    if (xhr.status === 200) { msg.className = 'small st-ok'; msg.textContent = '✓ Stored. Nothing is updated until you press Update pumps.'; $('#pfFile').value = ''; return; }
    let why = 'upload failed';
    try { why = JSON.parse(xhr.responseText).message || why; } catch (e) { /* not JSON */ }
    msg.className = 'small st-err';
    msg.textContent = '✗ ' + why;
  };
  xhr.onerror = () => { msg.className = 'small st-err'; msg.textContent = '✗ Upload failed (connection error).'; };
  xhr.open('POST', '/api/pump-firmware');
  xhr.send(fd);
});
$('#pfUpdate').addEventListener('click', async () => {
  const m = S.pumpFw;
  if (!m || !m.targets || !m.targets.length) return;
  const body = 'Install firmware ' + m.image.version + ' on ' + boardList(m.targets.map((t) => t.pumpId)) +
    '. This takes about ' + Math.ceil(m.targets.length * 0.85) + ' min, with no drinks until it finishes. If a board fails, the rest are left alone. Keep the power on.';
  if (!await confirmDlg('Update pumps?', body, 'Update', true)) return;
  send('pumpFwUpdate').catch((e) => toast(e.message || 'Update refused', 'err'));
});
$('#pfCancel').addEventListener('click', () => { send('pumpFwCancel').catch((e) => toast(e.message || 'Cancel failed', 'err')); });
$('#pfDelete').addEventListener('click', async () => {
  if (!await confirmDlg('Delete the stored file?', 'Pumps keep the firmware they have. You can upload a file again later.', 'Delete', true)) return;
  send('pumpFwDelete').catch((e) => toast(e.message || 'Delete failed', 'err'));
});

// OTA
let otaActive = false;
$('#otaGo').addEventListener('click', async () => {
  const file = $('#otaFile').files[0];
  if (!file) { toast('Choose a .bin file first', 'err'); return; }
  if (!/\.bin$/i.test(file.name)) { toast('Firmware must be a .bin file', 'err'); return; }
  if (isBusy()) { toast('Firmware update needs the controller idle', 'err'); return; }
  if (!await confirmDlg('Update firmware?', 'Upload ' + file.name + ' (' + Math.round(file.size / 1024) + ' KB). The controller reboots when it finishes. Do not power it off during the update.', 'Update', true)) return;
  const fd = new FormData();
  fd.append('firmware', file, file.name);
  const xhr = new XMLHttpRequest();
  otaActive = true;
  setOta(0, 'Uploading…');
  xhr.upload.onprogress = (e) => { if (e.lengthComputable) setOta(e.loaded / e.total * 100, 'Uploading… ' + Math.round(e.loaded / e.total * 100) + '%'); };
  xhr.onload = () => {
    otaActive = false;
    if (xhr.status === 200) setOta(100, '✓ Update written. The controller is rebooting…', 'st-ok');
    else if (xhr.status === 409) setOta(0, 'Rejected: the controller is busy.', 'st-err');
    else {
      let why = 'HTTP ' + xhr.status;
      try { const b = JSON.parse(xhr.responseText); why = b.error || b.message || why; } catch (e) { /* not JSON */ }
      setOta(0, '✗ Update failed: ' + why + '. The current firmware is still running.', 'st-err');
    }
  };
  xhr.onerror = () => { otaActive = false; setOta(0, 'Upload failed (connection error).', 'st-err'); };
  xhr.open('POST', '/api/ota');
  xhr.send(fd);
});
function setOta(pct, text, cls) { $('#otaBar').style.width = Math.max(0, Math.min(100, pct)) + '%'; const m = $('#otaMsg'); m.textContent = text; m.className = 'small ' + (cls || 'muted'); }
function onOta(m) {
  if (m.status === 'WRITING') setOta(m.percent, 'Writing flash… ' + m.percent + '%');
  else if (m.status === 'DONE') setOta(100, '✓ Update complete' + (m.message ? ' — ' + m.message : '. Rebooting…'), 'st-ok');
  else if (m.status === 'FAILED') setOta(m.percent || 0, '✗ ' + (m.message || 'Update failed') + '. The current firmware is still running.', 'st-err');
}

// Device info
function renderInfo(info) {
  if (info) S.info = info;
  const i = S.info || {}, h = S.hello || {};
  const rows = [['Firmware', i.fwVersion || h.fwVersion], ['Build', i.buildDate || h.buildDate], ['Hostname', h.hostname],
    ['Mode', (i.simulator != null ? i.simulator : h.simulator) ? 'Simulator' : 'Hardware'], ['IP', i.ip], ['Wi-Fi mode', i.wifiMode],
    ['Uptime', isNum(i.uptimeMs) ? fmtUptime(i.uptimeMs) : null], ['Free heap', isNum(i.freeHeap) ? Math.round(i.freeHeap / 1024) + ' KB' : null]];
  const dl = $('#sInfo');
  dl.innerHTML = '';
  rows.forEach(([k, v]) => dl.append(el('dt', { text: k }), el('dd', { text: v == null ? '—' : v })));
}
function fmtUptime(ms) { const s = Math.floor(ms / 1000); return Math.floor(s / 3600) + 'h ' + Math.floor(s / 60) % 60 + 'm ' + s % 60 + 's'; }
function fetchInfo() { fetch('/api/info').then((r) => r.json()).then(renderInfo).catch(() => { /* offline */ }); }
$('#sInfoRefresh').addEventListener('click', fetchInfo);

// ------------------------------------------------------------------ Maintenance (§15.7)
function maintPump() { const v = $('#mPump').value; return v === '' ? null : +v; }
function renderMaint() {
  const sel = $('#mPump');
  const ids = Array.from(new Set((S.config ? S.config.pumps.map((p) => p.pumpId) : []).concat(S.pumps.map((p) => p.pumpId)))).sort((a, b) => a - b);
  const sig = ids.map((id) => id + ':' + ((cfgPump(id) || {}).liquid || '')).join(',');
  if (sel.dataset.sig !== sig) {
    const cur = sel.value;
    sel.innerHTML = '';
    ids.forEach((id) => { const p = cfgPump(id); sel.append(el('option', { value: id, text: 'Pump ' + id + (p ? ' — ' + (p.liquid || '(unloaded)') : ' (not configured)') })); });
    if (ids.indexOf(+cur) >= 0) sel.value = cur;
    sel.dataset.sig = sig;
  }
  const rt = pumpRt(maintPump());
  $('#mPumpInfo').textContent = rt ? (rt.present ? 'Present' : 'Missing') + (rt.isBridge ? ' · bridge' : '') + ' · FW ' + (rt.fwVersion || '—') + ' · ' + fmt1(rt.weightG) + ' g' + (rt.activity !== 'IDLE' ? ' · ' + rt.activity : '') : '';
  const idle = S.connected && !isBusy();
  ['#mTare', '#mCal', '#mSetId', '#mLed', '#mRescan'].forEach((s) => setOff($(s), !idle || (s !== '#mRescan' && maintPump() == null)));
  $$('#mRawForm [data-needs-conn]').forEach((e) => setOff(e, !idle));
  const errs = (S.state && S.state.errors) || [];
  $('#mErrors').innerHTML = errs.length ? '<ul>' + errs.map((e) => '<li><b>' + esc(e.code) + (e.pumpId != null ? '(' + esc(e.pumpId) + ')' : '') + '</b> ' + esc(e.message) + ' <span class="muted">' + (e.latched ? 'latched' : SEVERITY[e.code] || '') + '</span></li>').join('') + '</ul>' : '<p class="muted">No active errors.</p>';
}
$('#mPump').addEventListener('change', renderMaint);
const withPump = (fn) => () => { const id = maintPump(); if (id == null) toast('Choose a pump', 'err'); else fn(id); };
$('#mTare').addEventListener('click', withPump(async (id) => {
  if (await confirmDlg('Tare pump ' + id + '?', 'Place the empty container on the scale first.', 'Tare')) cmd('pumpTare', { pumpId: id }, 'Pump ' + id + ' tared');
}));
$('#mCal').addEventListener('click', withPump(async (id) => {
  const g = Number($('#mGrams').value);
  if (!(g > 0)) { toast('Enter the known weight in grams', 'err'); return; }
  if (await confirmDlg('Calibrate pump ' + id + '?', 'Place the known ' + g + ' g weight on the scale first.', 'Calibrate')) cmd('pumpCalibrate', { pumpId: id, grams: g }, 'Pump ' + id + ' calibrated');
}));
$('#mSetId').addEventListener('click', withPump(async (id) => {
  const n = Number($('#mNewId').value);
  if (!isInt(n) || n < 1 || n > 254) { toast('New ID must be 1–254', 'err'); return; }
  if (n === id) return;
  if (await confirmDlg('Change pump ID?', 'Pump ' + id + ' will be given ID ' + n + ' (stored on the pump). The config is updated and the pumps are rescanned.', 'Set ID', true))
    cmd('pumpSetId', { pumpId: id, newId: n }, 'Pump ' + id + ' is now pump ' + n);
}));
$('#mLed').addEventListener('click', withPump((id) => cmd('pumpLedTest', { pumpId: id }, 'LED test on pump ' + id)));
$('#mRescan').addEventListener('click', () => cmd('rescan', null, 'Rescan done'));
$('#mClear').addEventListener('click', () => cmd('clearErrors', null, 'Latched errors cleared'));
$('#mRawForm').addEventListener('submit', (e) => {
  e.preventDefault();
  const id = maintPump(), c = $('#mRaw').value.trim();
  if (id == null || !c) return;
  const out = $('#mRawOut');
  const add = (t) => { out.textContent += t + '\n'; out.scrollTop = out.scrollHeight; };
  add('> [' + id + '] ' + c);
  send('pumpRaw', { pumpId: id, command: c }).then((d) => { ((d && d.lines) || []).forEach(add); },
    (err) => add('! ' + (err.code || '') + ' ' + (err.message || '')));
  $('#mRaw').value = '';
});
// Simulator fault injection (only shown when hello.simulator)
const fault = (f, extra) => withPump((id) => cmd('simFault', Object.assign({ pumpId: id, fault: f }, extra || {}), 'Fault set on pump ' + id));
$('#fMissing').addEventListener('click', fault('missing', { value: true }));
$('#fPresent').addEventListener('click', fault('missing', { value: false }));
$('#fTimeout').addEventListener('click', fault('timeout'));
$('#fOverrun').addEventListener('click', fault('overrun'));
$('#fSetWeight').addEventListener('click', () => { const w = Number($('#fWeight').value); if (!(w >= 0)) { toast('Weight must be ≥ 0', 'err'); return; } fault('weight', { weightG: w })(); });

// ------------------------------------------------------------------ start
window.drinkbot = { state: S, send, validateConfig }; // console debugging aid
const startTab = (location.hash || '').slice(1);
if (startTab && $('#tab-' + startTab)) showTab(startTab);
applyEnabled();
renderInfo();
connect();
})();
