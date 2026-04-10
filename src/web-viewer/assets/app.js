// minilog web viewer — frontend logic
// Plain ES2020, no build step, no dependencies.
'use strict';

// ── Constants ────────────────────────────────────────────────────────────────

const MAX_ROWS    = 2000;
const POLL_MS     = 500;
const BATCH       = 200;

const SEV_NAMES   = ['EMERG','ALERT','CRIT','ERR','WARN','NOTICE','INFO','DEBUG'];
const SEV_CODES   = { EMERG:0, ALERT:1, CRIT:2, ERR:3, WARN:4, NOTICE:5, INFO:6, DEBUG:7 };

const FAC_NAMES = [
  'kern','user','mail','daemon','auth','syslog','lpr','news',
  'uucp','clock','authpriv','ftp','ntp','audit','alert','clock2',
  'local0','local1','local2','local3','local4','local5','local6','local7',
];

const COLUMNS = [
  { key: 'time',     label: 'Msg Time' },
  { key: 'rcv',      label: 'Rcv Time',  hidden: true },
  { key: 'src',      label: 'Source' },
  { key: 'hostname', label: 'Hostname',  hidden: true },
  { key: 'app',      label: 'App' },
  { key: 'pid',      label: 'PID',       hidden: true },
  { key: 'sev',      label: 'Sev' },
  { key: 'proto',    label: 'Proto',     hidden: true },
  { key: 'msgid',    label: 'Msg ID',    hidden: true },
];

// ── State ────────────────────────────────────────────────────────────────────

let activeSink      = '';
let topOffset       = 0;
let tailOffset      = 0;
let atTail          = true;
let pollTimer       = 0;
let loadingUp       = false;
let clearOffset     = null;

let searchMatches      = [];
let searchTotalMatches = 0;
let matchIndex         = 0;
let pendingSearch      = '';

// Persisted filter state
let hiddenCols       = new Set();
let hiddenSeverities = new Set();
let activeFacilities = new Set();
let includePatterns  = [];
let excludePatterns  = [];

// Known facilities seen in stream (for chip population)
const knownFacilities = new Set();

// IntersectionObserver for upward infinite scroll
let topObserver = null;

// Currently expanded row (accordion — at most one)
let expandedRow = null;

// ── DOM refs ─────────────────────────────────────────────────────────────────

const sinkSelect    = document.getElementById('sink-select');
const logBody       = document.getElementById('log-body');
const mainEl        = document.getElementById('main');
const emptyState    = document.getElementById('empty-state');
const statusDot     = document.getElementById('status-dot');
const searchInput   = document.getElementById('search');
const topSentinel   = document.getElementById('top-sentinel');
const jumpTailBtn   = document.getElementById('jump-tail-btn');
const matchNav      = document.getElementById('match-nav');
const matchCounter  = document.getElementById('match-counter');
const matchPrev     = document.getElementById('match-prev');
const matchNext     = document.getElementById('match-next');
const matchClose    = document.getElementById('match-close');

// Dropdown elements
const dropdowns = {
  col:    { btn: document.getElementById('col-btn'),    panel: document.getElementById('col-panel') },
  sev:    { btn: document.getElementById('sev-btn'),    panel: document.getElementById('sev-panel') },
  fac:    { btn: document.getElementById('fac-btn'),    panel: document.getElementById('fac-panel') },
  filter: { btn: document.getElementById('filter-btn'), panel: document.getElementById('filter-panel') },
};

// ── localStorage ─────────────────────────────────────────────────────────────

const LS = {
  SINK:     'minilog-sink',
  COLS:     'minilog-cols-hidden',
  SEV:      'minilog-sev-hidden',
  FAC:      'minilog-fac-active',
  INC:      'minilog-inc',
  EXC:      'minilog-exc',
};

function lsGet(key, def) {
  try { const v = localStorage.getItem(key); return v !== null ? JSON.parse(v) : def; }
  catch { return def; }
}
function lsSet(key, val) {
  try { localStorage.setItem(key, JSON.stringify(val)); } catch {}
}

// ── Initialisation ────────────────────────────────────────────────────────────

async function init() {
  loadState();
  buildColChips();
  buildSevChips();
  buildFacChips();
  buildFilterPanel();
  initColumnResize();
  initRowExpansion();
  await Promise.all([loadVersion(), loadSinks()]);
  bindControls();
}

async function loadVersion() {
  try {
    const res = await fetch('/version');
    const data = await res.json();
    const el = document.getElementById('logo-version');
    if (el && data.version) el.textContent = 'v' + data.version;
  } catch { /* non-fatal */ }
}

function loadState() {
  hiddenCols       = new Set(lsGet(LS.COLS, COLUMNS.filter(c => c.hidden).map(c => c.key)));
  hiddenSeverities = new Set(lsGet(LS.SEV, []));
  activeFacilities = new Set(lsGet(LS.FAC, []));
  includePatterns  = lsGet(LS.INC, []);
  excludePatterns  = lsGet(LS.EXC, ['nodecom', 'Turning logging']);
}

// ── Sink loading ──────────────────────────────────────────────────────────────

async function loadSinks() {
  let sinks;
  try {
    const res = await fetch('/sinks');
    sinks = await res.json();
  } catch {
    setStatus('disconnected');
    return;
  }

  const savedSink = lsGet(LS.SINK, '');
  sinkSelect.innerHTML = '';
  for (const s of sinks) {
    const opt = document.createElement('option');
    opt.value = s.name;
    opt.textContent = s.name;
    if (s.name === savedSink) opt.selected = true;
    sinkSelect.appendChild(opt);
  }

  if (sinks.length > 0) {
    activeSink = sinkSelect.value || sinks[0].name;
    sinkSelect.value = activeSink;
    await loadTail();
  }
}

// ── Filter params ─────────────────────────────────────────────────────────────

function filterParams() {
  const p = new URLSearchParams();
  p.set('sink', activeSink);

  // Severity: send names of allowed levels (all names NOT in hiddenSeverities)
  const allowedSev = SEV_NAMES
    .filter(s => !hiddenSeverities.has(s));
  if (allowedSev.length < SEV_NAMES.length) {
    p.set('sev', allowedSev.map(s => s.toLowerCase()).join(','));
  }

  // Facility: send names of active facilities (empty = all)
  if (activeFacilities.size > 0) {
    p.set('fac', [...activeFacilities].join(','));
  }

  for (const p2 of includePatterns) p.append('inc', p2);
  for (const p2 of excludePatterns) p.append('exc', p2);

  if (clearOffset !== null) {
    p.set('since', String(clearOffset));
  }

  return p;
}

// ── Tail loading ──────────────────────────────────────────────────────────────

async function loadTail() {
  stopPoll();
  clearDOM();
  atTail = true;
  hideOverlays();
  pendingSearch      = '';
  searchMatches      = [];
  searchTotalMatches = 0;

  lsSet(LS.SINK, activeSink);

  const p = filterParams();
  p.set('tail', 'true');
  p.set('count', String(BATCH));

  let data;
  try {
    const res = await fetch('/lines?' + p);
    data = await res.json();
  } catch {
    setStatus('disconnected');
    startPoll();
    return;
  }

  setStatus('connected');
  tailOffset = data.tail_offset ?? 0;
  topOffset  = data.first_offset ?? 0;

  renderRows(data.lines ?? [], data.offsets ?? [], false);
  updateEmptyState();
  scrollToBottom();
  attachTopObserver();
  startPoll();
}

// ── Polling ───────────────────────────────────────────────────────────────────

function startPoll() {
  stopPoll();
  pollTimer = setInterval(pollTail, POLL_MS);
}

function stopPoll() {
  if (pollTimer) { clearInterval(pollTimer); pollTimer = 0; }
}

async function pollTail() {
  const p = filterParams();
  p.set('offset', String(tailOffset));
  p.set('count',  String(BATCH));
  p.set('dir',    'forward');

  let data;
  try {
    const res = await fetch('/lines?' + p);
    data = await res.json();
  } catch {
    setStatus('disconnected');
    return;
  }
  setStatus('connected');

  // Detect rotation: tail_offset regressed
  if ((data.tail_offset ?? 0) < tailOffset) {
    await loadTail();
    return;
  }

  tailOffset = data.tail_offset ?? tailOffset;

  const lines   = data.lines   ?? [];
  const offsets = data.offsets ?? [];
  if (lines.length === 0) return;

  const wasNearBottom = isNearBottom();
  renderRows(lines, offsets, true);
  pruneTop();
  updateEmptyState();

  if (atTail && wasNearBottom) scrollToBottom();
}

// ── Upward scroll (load older) ────────────────────────────────────────────────

function attachTopObserver() {
  if (topObserver) topObserver.disconnect();
  if (topOffset === 0) return; // nothing older to load

  topObserver = new IntersectionObserver(entries => {
    if (entries[0].isIntersecting) loadOlder();
  }, { root: mainEl, threshold: 0 });

  // Observe the first table row if it exists, else the sentinel
  const firstRow = logBody.firstElementChild;
  topObserver.observe(firstRow || topSentinel);
}

async function loadOlder() {
  if (loadingUp || topOffset === 0) return;
  loadingUp = true;
  atTail = false;
  showJumpTail();

  const p = filterParams();
  p.set('offset', String(topOffset));
  p.set('count',  String(BATCH));
  p.set('dir',    'backward');

  let data;
  try {
    const res = await fetch('/lines?' + p);
    data = await res.json();
  } catch {
    loadingUp = false;
    return;
  }

  const lines   = data.lines   ?? [];
  const offsets = data.offsets ?? [];

  if (lines.length > 0) {
    const prevHeight = mainEl.scrollHeight;
    prependRows(lines, offsets);
    // Restore scroll position so the viewport doesn't jump
    mainEl.scrollTop += mainEl.scrollHeight - prevHeight;
    topOffset = data.first_offset ?? topOffset;
    updateEmptyState();
  }

  // Re-attach observer to the new first row
  if (topOffset === 0) {
    if (topObserver) topObserver.disconnect();
  } else {
    attachTopObserver();
  }

  loadingUp = false;
}

// ── Row rendering ─────────────────────────────────────────────────────────────

function renderRows(lines, offsets, isNew) {
  for (let i = 0; i < lines.length; i++) {
    const entry = tryParse(lines[i]);
    if (!entry) continue;
    appendRow(entry, isNew, offsets[i] ?? 0, lines[i]);
  }
}

function prependRows(lines, offsets) {
  // Build fragment oldest-first, then insert before current first child
  const frag = document.createDocumentFragment();
  for (let i = 0; i < lines.length; i++) {
    const entry = tryParse(lines[i]);
    if (!entry) continue;
    const tr = buildRow(entry, false, offsets[i] ?? 0, lines[i]);
    frag.appendChild(tr);
  }
  logBody.insertBefore(frag, logBody.firstChild);
}

function appendRow(entry, isNew, offset, rawLine) {
  const tr = buildRow(entry, isNew, offset, rawLine);
  logBody.appendChild(tr);
  if (isNew) {
    tr.addEventListener('animationend', () => tr.classList.remove('row-new'), { once: true });
  }
}

function buildRow(entry, isNew, offset, rawLine) {
  const sev = sevLabel(entry.severity);
  const fac = facLabel(entry.facility);

  // Register facility chip if new
  if (fac && !knownFacilities.has(fac)) {
    knownFacilities.add(fac);
    addFacilityChip(fac);
  }

  const tr = document.createElement('tr');
  tr.dataset.raw     = (rawLine ?? JSON.stringify(entry)).toLowerCase();
  tr.dataset.rawJson = rawLine ?? JSON.stringify(entry);
  tr.dataset.sev     = sev;
  tr.dataset.offset  = String(offset);

  if (isNew) tr.classList.add('row-new');

  tr.innerHTML = `
    <td class="col-time"    >${escHtml(entry.msg_time ?? '')}</td>
    <td class="col-rcv"     >${escHtml(formatTime(entry.rcv))}</td>
    <td class="col-src"      title="${escHtml(entry.src ?? '')}">${escHtml(entry.src ?? '')}</td>
    <td class="col-hostname" title="${escHtml(entry.hostname ?? '')}">${escHtml(entry.hostname ?? '')}</td>
    <td class="col-app"      title="${escHtml(entry.app ?? '')}">${escHtml(entry.app ?? '')}</td>
    <td class="col-pid"     >${escHtml(entry.pid != null ? String(entry.pid) : '')}</td>
    <td class="col-sev"     >${badge(sev)}</td>
    <td class="col-proto"   >${escHtml(entry.proto ?? '')}</td>
    <td class="col-msgid"   >${escHtml(entry.msgid ?? '')}</td>
    <td class="col-msg"      title="${escHtml(entry.message ?? '')}">${escHtml(entry.message ?? '')}</td>
  `;

  // Apply column visibility
  for (const key of hiddenCols) {
    const td = tr.querySelector(`td.col-${key}`);
    if (td) td.classList.add('col-hidden');
  }

  // Apply search match highlight if this row's offset is a known search result.
  if (pendingSearch && searchMatches.some(m => m.offset === parseInt(tr.dataset.offset, 10))) {
    tr.classList.add('row-match');
  }

  return tr;
}

function pruneTop() {
  while (logBody.children.length > MAX_ROWS) {
    const removed = logBody.removeChild(logBody.firstChild);
    // Advance topOffset by the byte size of the removed line — we don't know
    // the exact size, so re-read topOffset from the next row's offset dataset.
    const next = logBody.firstChild;
    if (next && next.dataset && next.dataset.offset) {
      topOffset = parseInt(next.dataset.offset, 10);
    }
  }
}

// ── Server search ─────────────────────────────────────────────────────────────

async function runServerSearch(q) {
  if (!q) return;

  const p = filterParams();
  p.set('q',     q);
  p.set('limit', String(BATCH));

  let data;
  try {
    const res = await fetch('/search?' + p);
    data = await res.json();
  } catch {
    return;
  }

  searchMatches      = data.results ?? [];
  searchTotalMatches = data.total_matches ?? searchMatches.length;
  matchIndex         = 0;
  pendingSearch      = q;

  if (searchMatches.length === 0) {
    matchCounter.textContent = 'No matches';
    showMatchNav();
    return;
  }

  showMatchNav();
  await jumpToMatch(0);
}

async function jumpToMatch(idx) {
  if (searchMatches.length === 0) return;
  // Wrap around at both ends.
  idx = ((idx % searchMatches.length) + searchMatches.length) % searchMatches.length;
  matchIndex = idx;

  const shown = searchMatches.length;
  const total = searchTotalMatches;
  matchCounter.textContent = total > shown
    ? `${idx + 1} / ${shown} (of ${total})`
    : `${idx + 1} / ${total}`;

  const match = searchMatches[idx];

  // Remove previous current highlight
  for (const tr of logBody.querySelectorAll('.row-current')) {
    tr.classList.remove('row-current');
  }

  // Look for the row in the current DOM by offset (handles duplicate lines).
  let found = null;
  for (const tr of logBody.rows) {
    if (parseInt(tr.dataset.offset, 10) === match.offset) {
      found = tr;
      break;
    }
  }

  if (found) {
    found.classList.add('row-match', 'row-current');
    found.scrollIntoView({ block: 'center', behavior: 'smooth' });
    atTail = false;
    return;
  }

  // Not in DOM — fetch a context window around the match offset
  stopPoll();
  clearDOM();
  atTail = false;

  const p = filterParams();
  p.set('offset', String(match.offset));
  p.set('count',  String(BATCH));
  p.set('dir',    'forward');

  let data;
  try {
    const res = await fetch('/lines?' + p);
    data = await res.json();
  } catch { return; }

  tailOffset = data.tail_offset ?? tailOffset;
  topOffset  = data.first_offset ?? match.offset;

  renderRows(data.lines ?? [], data.offsets ?? [], false);
  updateEmptyState();
  attachTopObserver();
  startPoll();

  // Re-highlight after render, again by offset.
  for (const tr of logBody.rows) {
    if (parseInt(tr.dataset.offset, 10) === match.offset) {
      tr.classList.add('row-match', 'row-current');
      tr.scrollIntoView({ block: 'center', behavior: 'smooth' });
      break;
    }
  }
}

// ── Column visibility ─────────────────────────────────────────────────────────

function buildColChips() {
  const container = document.getElementById('col-chips');
  container.innerHTML = '';
  for (const col of COLUMNS) {
    const chip = makeToggleChip(col.label, !hiddenCols.has(col.key), () => {
      if (hiddenCols.has(col.key)) hiddenCols.delete(col.key);
      else hiddenCols.add(col.key);
      lsSet(LS.COLS, [...hiddenCols]);
      applyColVisibility();
    });
    container.appendChild(chip);
  }
  applyColVisibility();
}

function applyColVisibility() {
  for (const col of COLUMNS) {
    const hide = hiddenCols.has(col.key);
    const th = document.querySelector(`th[data-col="${col.key}"]`);
    if (th) th.classList.toggle('col-hidden', hide);
    for (const td of document.querySelectorAll(`#log-body td.col-${col.key}`)) {
      td.classList.toggle('col-hidden', hide);
    }
  }
}

// ── Severity dropdown ─────────────────────────────────────────────────────────

function buildSevChips() {
  const container = document.getElementById('sev-chips');
  container.innerHTML = '';
  for (const sev of SEV_NAMES) {
    const active = !hiddenSeverities.has(sev);
    const chip = document.createElement('span');
    chip.className = 'chip' + (active ? ' active' : '');
    chip.dataset.sev = sev;
    chip.textContent = sev;
    chip.setAttribute('role', 'checkbox');
    chip.setAttribute('aria-checked', String(active));
    chip.addEventListener('click', () => {
      if (hiddenSeverities.has(sev)) hiddenSeverities.delete(sev);
      else hiddenSeverities.add(sev);
      chip.classList.toggle('active', !hiddenSeverities.has(sev));
      chip.setAttribute('aria-checked', String(!hiddenSeverities.has(sev)));
      lsSet(LS.SEV, [...hiddenSeverities]);
      updateSevBadge();
      onFilterChange();
    });
    container.appendChild(chip);
  }
  updateSevBadge();
}

function updateSevBadge() {
  const badge = document.getElementById('sev-badge');
  const count = hiddenSeverities.size;
  badge.hidden = count === 0;
  badge.textContent = String(count);
}

// ── Facility dropdown ─────────────────────────────────────────────────────────

function buildFacChips() {
  const container = document.getElementById('fac-chips');
  container.innerHTML = '';

  // "All" chip
  const allChip = document.createElement('span');
  allChip.className = 'chip chip-all' + (activeFacilities.size === 0 ? ' active' : '');
  allChip.textContent = 'All';
  allChip.setAttribute('role', 'checkbox');
  allChip.setAttribute('aria-checked', String(activeFacilities.size === 0));
  allChip.addEventListener('click', () => {
    activeFacilities.clear();
    lsSet(LS.FAC, []);
    updateFacChips();
    onFilterChange();
  });
  container.appendChild(allChip);

  // One chip per known facility, alphabetically
  for (const fac of [...knownFacilities].sort()) {
    container.appendChild(makeFacChip(fac));
  }

  updateFacBadge();
}

function makeFacChip(fac) {
  const active = activeFacilities.has(fac);
  const chip = document.createElement('span');
  chip.className = 'chip' + (active ? ' active' : '');
  chip.textContent = fac;
  chip.dataset.fac = fac;
  chip.setAttribute('role', 'checkbox');
  chip.setAttribute('aria-checked', String(active));
  chip.addEventListener('click', () => {
    if (activeFacilities.has(fac)) activeFacilities.delete(fac);
    else activeFacilities.add(fac);
    lsSet(LS.FAC, [...activeFacilities]);
    updateFacChips();
    onFilterChange();
  });
  return chip;
}

function addFacilityChip(fac) {
  if (!fac) return;
  // Chip already exists in the panel
  const container = document.getElementById('fac-chips');
  if (container.querySelector(`[data-fac="${fac}"]`)) return;
  // Insert alphabetically after the "All" chip
  const chips = [...container.querySelectorAll('[data-fac]')];
  const newChip = makeFacChip(fac);
  const after = chips.find(c => c.dataset.fac > fac);
  if (after) container.insertBefore(newChip, after);
  else container.appendChild(newChip);
  updateFacBadge();
}

function updateFacChips() {
  const container = document.getElementById('fac-chips');
  for (const chip of container.querySelectorAll('.chip')) {
    const fac = chip.dataset.fac;
    if (!fac) {
      const active = activeFacilities.size === 0;
      chip.classList.toggle('active', active);
      chip.setAttribute('aria-checked', String(active));
    } else {
      const active = activeFacilities.has(fac);
      chip.classList.toggle('active', active);
      chip.setAttribute('aria-checked', String(active));
    }
  }
  updateFacBadge();
}

function updateFacBadge() {
  const badge = document.getElementById('fac-badge');
  badge.hidden = activeFacilities.size === 0;
  badge.textContent = String(activeFacilities.size);
}

// ── Filters dropdown ──────────────────────────────────────────────────────────

function buildFilterPanel() {
  rebuildPatternChips('inc', includePatterns, 'chip-pattern--inc');
  rebuildPatternChips('exc', excludePatterns, 'chip-pattern--exc');
  updateFilterBadge();
}

function rebuildPatternChips(type, patterns, cls) {
  const container = document.getElementById(`${type}-chips`);
  container.innerHTML = '';
  for (let i = 0; i < patterns.length; i++) {
    container.appendChild(makePatternChip(type, i, patterns[i], cls));
  }
}

function makePatternChip(type, idx, text, cls) {
  const span = document.createElement('span');
  span.className = `chip-pattern ${cls}`;
  span.textContent = text;
  const del = document.createElement('button');
  del.className = 'chip-del';
  del.textContent = '✕';
  del.title = 'Remove';
  del.addEventListener('click', () => {
    if (type === 'inc') includePatterns.splice(idx, 1);
    else excludePatterns.splice(idx, 1);
    lsSet(type === 'inc' ? LS.INC : LS.EXC, type === 'inc' ? includePatterns : excludePatterns);
    buildFilterPanel();
    onFilterChange();
  });
  span.appendChild(del);
  return span;
}

function addPattern(type) {
  const input = document.getElementById(`${type}-input`);
  const val = input.value.trim();
  if (!val) return;
  if (type === 'inc') includePatterns.push(val);
  else excludePatterns.push(val);
  lsSet(type === 'inc' ? LS.INC : LS.EXC, type === 'inc' ? includePatterns : excludePatterns);
  input.value = '';
  buildFilterPanel();
  onFilterChange();
}

function updateFilterBadge() {
  const badge = document.getElementById('filter-badge');
  const count = includePatterns.length + excludePatterns.length;
  badge.hidden = count === 0;
  badge.textContent = String(count);
}

// ── Filter change → reload tail ───────────────────────────────────────────────

function onFilterChange() {
  searchMatches      = [];
  searchTotalMatches = 0;
  pendingSearch      = '';
  hideOverlays();
  loadTail();
}

// ── Dropdown open/close ───────────────────────────────────────────────────────

function openDropdown(key) {
  // Close all others first
  for (const [k, d] of Object.entries(dropdowns)) {
    if (k !== key) {
      d.panel.hidden = true;
      d.btn.setAttribute('aria-expanded', 'false');
    }
  }
  const d = dropdowns[key];
  d.panel.hidden = false;
  d.btn.setAttribute('aria-expanded', 'true');
}

function closeDropdown(key) {
  const d = dropdowns[key];
  d.panel.hidden = true;
  d.btn.setAttribute('aria-expanded', 'false');
}

function closeAllDropdowns() {
  for (const key of Object.keys(dropdowns)) closeDropdown(key);
}

function toggleDropdown(key) {
  if (dropdowns[key].panel.hidden) openDropdown(key);
  else closeDropdown(key);
}

// ── Control bindings ──────────────────────────────────────────────────────────

function resetSettings() {
  clearOffset = null;
  [LS.COLS, LS.SEV, LS.FAC, LS.INC, LS.EXC].forEach(k => localStorage.removeItem(k));
  loadState();
  buildColChips();
  buildSevChips();
  knownFacilities.clear();
  buildFacChips();
  buildFilterPanel();
  clearSearch();
  onFilterChange();
}

function bindControls() {
  document.getElementById('reset-btn').addEventListener('click', resetSettings);
  document.getElementById('clear-btn').addEventListener('click', clearView);

  sinkSelect.addEventListener('change', () => {
    activeSink = sinkSelect.value;
    clearOffset = null;
    knownFacilities.clear();
    buildFacChips();
    onFilterChange();
  });

  for (const key of Object.keys(dropdowns)) {
    dropdowns[key].btn.addEventListener('click', e => {
      e.stopPropagation();
      toggleDropdown(key);
    });
  }

  document.addEventListener('click', e => {
    const t = e.target;
    for (const [key, d] of Object.entries(dropdowns)) {
      if (!d.btn.closest('.dropdown').contains(t)) closeDropdown(key);
    }
  });

  document.addEventListener('keydown', e => {
    if (e.key === 'Escape') {
      closeAllDropdowns();
      if (pendingSearch) clearSearch();
    }
  });

  // Search input
  searchInput.addEventListener('keydown', e => {
    if (e.key === 'Enter') {
      const q = searchInput.value.trim();
      if (searchMatches.length > 0 && q === pendingSearch)
        jumpToMatch(matchIndex + 1);
      else
        runServerSearch(q);
    }
  });

  // Filter add buttons and Enter key in inputs
  document.getElementById('inc-add').addEventListener('click', () => addPattern('inc'));
  document.getElementById('exc-add').addEventListener('click', () => addPattern('exc'));
  document.getElementById('inc-input').addEventListener('keydown', e => { if (e.key === 'Enter') addPattern('inc'); });
  document.getElementById('exc-input').addEventListener('keydown', e => { if (e.key === 'Enter') addPattern('exc'); });

  // Match nav
  matchPrev.addEventListener('click', () => jumpToMatch(matchIndex - 1));
  matchNext.addEventListener('click', () => jumpToMatch(matchIndex + 1));
  matchClose.addEventListener('click', clearSearch);

  // Jump to tail button
  jumpTailBtn.addEventListener('click', () => loadTail());

  // Scroll tracking — detect when user is at the bottom
  mainEl.addEventListener('scroll', () => {
    if (isNearBottom()) {
      atTail = true;
      if (!pendingSearch) hideOverlays();
    } else {
      if (!pendingSearch) showJumpTail();
    }
  });
}

function clearSearch() {
  searchMatches      = [];
  searchTotalMatches = 0;
  matchIndex         = 0;
  pendingSearch      = '';
  searchInput.value = '';
  // Remove match highlights
  for (const tr of logBody.querySelectorAll('.row-match, .row-current')) {
    tr.classList.remove('row-match', 'row-current');
  }
  hideOverlays();
  if (!atTail) showJumpTail();
}

function clearView() {
  clearOffset = tailOffset;
  clearSearch();
  stopPoll();
  clearDOM();
  atTail = true;
  hideOverlays();
  updateEmptyState();
  startPoll();
}

// ── Overlay helpers ───────────────────────────────────────────────────────────

function showJumpTail() {
  if (pendingSearch) return; // match-nav has priority
  jumpTailBtn.hidden = false;
}

function showMatchNav() {
  matchNav.hidden    = false;
  jumpTailBtn.hidden = true;
}

function hideOverlays() {
  jumpTailBtn.hidden = true;
  matchNav.hidden    = true;
}

// ── DOM helpers ───────────────────────────────────────────────────────────────

function clearDOM() {
  logBody.innerHTML = '';
  topOffset = 0;
  expandedRow = null;
  if (topObserver) { topObserver.disconnect(); topObserver = null; }
}

function scrollToBottom() {
  mainEl.scrollTop = mainEl.scrollHeight;
}

function isNearBottom() {
  return mainEl.scrollHeight - mainEl.scrollTop - mainEl.clientHeight < 80;
}

function updateEmptyState() {
  emptyState.hidden = logBody.rows.length > 0;
}

function setStatus(state) {
  statusDot.className = 'status-dot ' + state;
  statusDot.title = state === 'connected' ? 'Connected' : 'Disconnected';
}

// ── Chip factory ──────────────────────────────────────────────────────────────

function makeToggleChip(label, active, onClick) {
  const chip = document.createElement('span');
  chip.className = 'chip' + (active ? ' active' : '');
  chip.textContent = label;
  chip.setAttribute('role', 'checkbox');
  chip.setAttribute('aria-checked', String(active));
  chip.addEventListener('click', () => {
    onClick();
    const nowActive = chip.classList.toggle('active');
    chip.setAttribute('aria-checked', String(nowActive));
  });
  return chip;
}

// ── Helpers ───────────────────────────────────────────────────────────────────

function tryParse(s) {
  try { return JSON.parse(s); } catch { return null; }
}

function sevLabel(val) {
  if (val == null) return 'UNKNOWN';
  if (typeof val === 'string') return val.toUpperCase();
  return SEV_NAMES[val] ?? 'UNKNOWN';
}

function facLabel(val) {
  if (val == null) return 'unknown';
  if (typeof val === 'string') return val.toLowerCase();
  return FAC_NAMES[val] ?? `fac${val}`;
}

function badge(sev) {
  return `<span class="badge badge-${sev.toLowerCase()}">${escHtml(sev)}</span>`;
}

function formatTime(iso) {
  if (!iso) return '';
  try {
    const d = new Date(iso);
    const p = n => String(n).padStart(2, '0');
    return `${d.getFullYear()}-${p(d.getMonth()+1)}-${p(d.getDate())} ` +
           `${p(d.getHours())}:${p(d.getMinutes())}:${p(d.getSeconds())}`;
  } catch { return iso; }
}

const escMap = { '&':'&amp;', '<':'&lt;', '>':'&gt;', '"':'&quot;', "'":'&#39;' };
function escHtml(s) {
  return String(s).replace(/[&<>"']/g, c => escMap[c]);
}

// ── Row expansion (detail panel) ──────────────────────────────────────────────

const DETAIL_FIELDS = [
  ['rcv',      'Rcv Time'],
  ['src',      'Source'],
  ['proto',    'Proto'],
  ['facility', 'Facility'],
  ['severity', 'Severity'],
  ['hostname', 'Hostname'],
  ['app',      'App'],
  ['pid',      'PID'],
  ['msgid',    'Msg ID'],
  ['msg_time', 'Msg Time'],
  ['message',  'Message'],
];

function toggleDetail(tr) {
  // Already expanded — collapse it
  if (tr.classList.contains('row-expanded')) {
    collapseDetail(tr);
    return;
  }

  // Accordion: collapse any other expanded row
  if (expandedRow) collapseDetail(expandedRow);

  let entry;
  try { entry = JSON.parse(tr.dataset.rawJson); } catch { entry = null; }

  if (!entry) return;

  // Build detail row
  const colCount = document.querySelectorAll('#log-table thead th').length;
  const detailTr = document.createElement('tr');
  detailTr.className = 'detail-row';
  const td = document.createElement('td');
  td.colSpan = colCount;
  td.className = 'detail-cell';

  const grid = document.createElement('div');
  grid.className = 'detail-grid';
  for (const [key, label] of DETAIL_FIELDS) {
    const k = document.createElement('div');
    k.className = 'detail-key';
    k.textContent = label;
    const v = document.createElement('div');
    v.className = 'detail-val';
    const val = entry[key];
    v.textContent = val != null ? String(val) : '—';
    grid.appendChild(k);
    grid.appendChild(v);
  }
  td.appendChild(grid);
  detailTr.appendChild(td);

  tr.after(detailTr);
  tr.classList.add('row-expanded');
  expandedRow = tr;
}

function collapseDetail(tr) {
  const next = tr.nextElementSibling;
  if (next && next.classList.contains('detail-row')) next.remove();
  tr.classList.remove('row-expanded');
  if (expandedRow === tr) expandedRow = null;
}

function initRowExpansion() {
  logBody.addEventListener('click', e => {
    // Don't toggle when clicking a resize handle
    if (e.target.closest('.col-resize-handle')) return;
    const tr = e.target.closest('tr');
    if (!tr || tr.classList.contains('detail-row')) return;
    toggleDetail(tr);
  });
}

// ── Column resizing ───────────────────────────────────────────────────────────

function initColumnResize() {
  const table = document.getElementById('log-table');
  const ths = table.querySelectorAll('thead th');

  for (const th of ths) {
    const handle = document.createElement('div');
    handle.className = 'col-resize-handle';
    th.appendChild(handle);

    let startX, startW;

    handle.addEventListener('mousedown', e => {
      e.preventDefault();
      startX = e.clientX;
      startW = th.offsetWidth;
      handle.classList.add('active');

      const onMove = ev => {
        const w = Math.max(40, startW + ev.clientX - startX);
        th.style.width = w + 'px';
      };
      const onUp = () => {
        handle.classList.remove('active');
        document.removeEventListener('mousemove', onMove);
        document.removeEventListener('mouseup', onUp);
      };
      document.addEventListener('mousemove', onMove);
      document.addEventListener('mouseup', onUp);
    });
  }
}

// ── Bootstrap ─────────────────────────────────────────────────────────────────
init();
