(function () {
  'use strict';

  // ---- Constants ----
  // JITTER_STABLE_FRAMES must match JITTER_PREFILL_FRAMES in lib/config/include/config/build.h
  const JITTER_STABLE_FRAMES = 14;
  const MIXER_SCHEMA_VERSION = 2;
  const MIXER_MAX_STREAMS = 4;
  const MIXER_GAIN_MIN_PCT = 0;
  const MIXER_GAIN_MAX_PCT = 400;

  // ---- Top-level state ----

  const state = {
    ts: 0,
    self: '',
    nodes: [],
    heapKb: null,
    core0LoadPct: null,
    latencyMs: null,
    netIf: 'usb_ncm (10.48.0.1)',
    buildLabel: '--',
    meshState: 'Mesh --',
    bpm: null,
    fftBins: null,
    monitor: [],
    ota: null,
    uplink: null,
    mixer: null
  };

  const mixerState = {
    schemaVersion: MIXER_SCHEMA_VERSION,
    outGainPct: 200,
    streamCount: MIXER_MAX_STREAMS,
    streams: [],
    jitterFrames: -1,
    inputMode: 'aux'
  };

  const demoRuntime = {
    intervalId: null,
    tick: 0,
    uplink: {
      enabled: false,
      configured: false,
      rootApplied: false,
      pendingApply: false,
      ssid: '',
      lastError: '',
      updatedMs: 0
    },
    mixer: {
      schemaVersion: MIXER_SCHEMA_VERSION,
      outGainPct: 200,
      streamCount: MIXER_MAX_STREAMS,
      streams: [
        { id: 1, gainPct: 100, enabled: true, muted: false, solo: false, active: true },
        { id: 2, gainPct: 100, enabled: false, muted: false, solo: false, active: false },
        { id: 3, gainPct: 100, enabled: false, muted: false, solo: false, active: false },
        { id: 4, gainPct: 100, enabled: false, muted: false, solo: false, active: false }
      ],
      jitterFrames: -1,
      inputMode: 'aux'
    }
  };

  let ws = null;
  let wsAttempts = 0;
  let reconnectTimer = null;
  let demoMode = false;
  let selectedMac = null;
  let mixerSendTimer = null;
  const forceDemoMode = document.body?.dataset?.forceDemo === '1' ||
    new URLSearchParams(window.location.search).get('demo') === '1';

  const topologyCanvas = document.getElementById('topology-canvas');
  const topologyCtx = topologyCanvas.getContext('2d');
  const fftCanvas = document.getElementById('fft-canvas');
  const fftCtx = fftCanvas ? fftCanvas.getContext('2d') : null;

  const $ = (id) => document.getElementById(id);
  const connDot = $('conn-dot');
  const connLabel = $('conn-label');
  const wsDot = $('ws-dot');
  const wsLabel = $('ws-label');

  const dpr = window.devicePixelRatio || 1;
  let nodePositions = {};
  let animTime = 0;

  // ---- Utilities ----

  function setConnState(kind, label) {
    connDot.className = `status-dot status-${kind}`;
    wsDot.className = `status-dot status-${kind}`;
    connLabel.textContent = label;
    wsLabel.textContent = kind === 'connected' ? 'WS Active' : 'WS Inactive';
  }

  function shortMac(mac) { return mac ? mac.slice(-8) : '--'; }

  function formatUptime(ms) {
    const s = Math.floor((ms || 0) / 1000);
    const h = Math.floor(s / 3600);
    const m = Math.floor((s % 3600) / 60);
    const r = s % 60;
    return `${h}h ${String(m).padStart(2, '0')}m ${String(r).padStart(2, '0')}s`;
  }

  function numOrNA(value, suffix = '') {
    return typeof value === 'number' && Number.isFinite(value) ? `${value}${suffix}` : 'N/A';
  }

  function formatDb(db) {
    if (db <= -60) return '-∞ dB';
    return (db >= 0 ? '+' : '') + db.toFixed(1) + ' dB';
  }

  function clampInt(value, min, max, fallback) {
    const n = Number(value);
    if (!Number.isFinite(n)) return fallback;
    return Math.max(min, Math.min(max, Math.round(n)));
  }

  function pctFromLegacyDb(db, fallbackPct = 200) {
    const n = Number(db);
    if (!Number.isFinite(n)) return fallbackPct;
    if (n <= -60) return MIXER_GAIN_MIN_PCT;
    const linear = Math.pow(10, n / 20);
    return clampInt(linear * 100, MIXER_GAIN_MIN_PCT, MIXER_GAIN_MAX_PCT, fallbackPct);
  }

  function makeDefaultStream(id) {
    return {
      id,
      gainPct: 100,
      enabled: id === 1,
      muted: false,
      solo: false,
      active: id === 1
    };
  }

  function makeDefaultMixerState() {
    return {
      schemaVersion: MIXER_SCHEMA_VERSION,
      outGainPct: 200,
      streamCount: MIXER_MAX_STREAMS,
      streams: Array.from({ length: MIXER_MAX_STREAMS }, (_, idx) => makeDefaultStream(idx + 1)),
      jitterFrames: -1,
      inputMode: 'aux'
    };
  }

  Object.assign(mixerState, makeDefaultMixerState());

  // ---- Normalization ----

  function normalizeNode(rawNode, fallbackRootMac) {
    if (!rawNode || typeof rawNode !== 'object') return null;
    const mac = typeof rawNode.mac === 'string' ? rawNode.mac : null;
    if (!mac) return null;
    const root = !!rawNode.root;
    const normalizedLayer = Number.isFinite(rawNode.layer)
      ? Math.max(0, Math.floor(rawNode.layer))
      : (root ? 0 : 1);
    const parent = typeof rawNode.parent === 'string'
      ? rawNode.parent
      : (!root && fallbackRootMac && fallbackRootMac !== mac ? fallbackRootMac : null);
    return {
      mac,
      role: typeof rawNode.role === 'string' ? rawNode.role : 'RX',
      root,
      layer: normalizedLayer,
      rssi: Number.isFinite(rawNode.rssi) ? rawNode.rssi : -100,
      children: Number.isFinite(rawNode.children) ? rawNode.children : 0,
      streaming: !!rawNode.streaming,
      parent,
      uptime: Number.isFinite(rawNode.uptime) ? rawNode.uptime : 0,
      stale: !!rawNode.stale
    };
  }

  function normalizeMixer(raw) {
    if (!raw || typeof raw !== 'object') return null;
    const normalized = makeDefaultMixerState();
    normalized.schemaVersion = Number.isFinite(raw.schemaVersion) ? raw.schemaVersion : MIXER_SCHEMA_VERSION;
    normalized.outGainPct = Number.isFinite(raw.outGainPct)
      ? clampInt(raw.outGainPct, MIXER_GAIN_MIN_PCT, MIXER_GAIN_MAX_PCT, 200)
      : pctFromLegacyDb(raw.outputGainDb, 200);
    normalized.jitterFrames = Number.isFinite(raw.jitterFrames) ? raw.jitterFrames : -1;
    normalized.inputMode = typeof raw.inputMode === 'string' ? raw.inputMode : 'aux';

    const rawStreams = Array.isArray(raw.streams) ? raw.streams : [];
    if (rawStreams.length) {
      const byId = new Map();
      rawStreams.forEach((stream) => {
        const id = clampInt(stream?.id ?? stream?.stream_id, 1, MIXER_MAX_STREAMS, 0);
        if (!id) return;
        byId.set(id, {
          id,
          gainPct: clampInt(stream.gainPct ?? stream.gain_pct, MIXER_GAIN_MIN_PCT, MIXER_GAIN_MAX_PCT, 100),
          enabled: !!stream.enabled,
          muted: !!stream.muted,
          solo: !!stream.solo,
          active: !!stream.active
        });
      });
      normalized.streams = normalized.streams.map((fallback) => byId.get(fallback.id) || fallback);
    }

    // Legacy payload compatibility: carry old mute into stream-1 when streams[] is absent.
    if (!rawStreams.length && typeof raw.outputMute === 'boolean') {
      normalized.streams[0].muted = !!raw.outputMute;
    }

    normalized.streamCount = normalized.streams.length;
    return normalized;
  }

  function normalizePayload(msg) {
    const rawNodes = Array.isArray(msg?.nodes) ? msg.nodes : [];
    const rootNode = rawNodes.find((n) => n && n.root && typeof n.mac === 'string');
    const selfMac = typeof msg?.self === 'string' ? msg.self : '';
    const fallbackRootMac = rootNode?.mac || selfMac || null;

    const dedup = new Map();
    rawNodes.forEach((n) => {
      const normalized = normalizeNode(n, fallbackRootMac);
      if (normalized) dedup.set(normalized.mac, normalized);
    });
    const nodes = Array.from(dedup.values()).sort((a, b) => a.mac.localeCompare(b.mac));

    return {
      ts: Number.isFinite(msg?.ts) ? msg.ts : 0,
      self: selfMac,
      nodes,
      heapKb: Number.isFinite(msg?.heapKb) ? msg.heapKb : null,
      core0LoadPct: Number.isFinite(msg?.core0LoadPct) ? msg.core0LoadPct : null,
      latencyMs: Number.isFinite(msg?.latencyMs) ? msg.latencyMs : null,
      netIf: typeof msg?.netIf === 'string' ? msg.netIf : state.netIf,
      buildLabel: typeof msg?.buildLabel === 'string' ? msg.buildLabel : state.buildLabel,
      meshState: typeof msg?.meshState === 'string' ? msg.meshState : state.meshState,
      bpm: Number.isFinite(msg?.bpm) ? msg.bpm : null,
      fftBins: Array.isArray(msg?.fftBins) ? msg.fftBins : null,
      monitor: Array.isArray(msg?.monitor) ? msg.monitor : [],
      ota: msg?.ota && typeof msg.ota === 'object' ? msg.ota : null,
      uplink: msg?.uplink && typeof msg.uplink === 'object' ? msg.uplink : null,
      mixer: msg?.mixer && typeof msg.mixer === 'object' ? normalizeMixer(msg.mixer) : null
    };
  }

  // ---- Uplink UI ----

  function updateUplinkStatus() {
    const statusEl = $('uplink-status');
    if (!statusEl) return;
    const u = state.uplink;
    if (!u) {
      statusEl.textContent = 'Uplink status unavailable';
      return;
    }
    const parts = [u.enabled ? 'Enabled' : 'Disabled'];
    if (u.ssid) parts.push(`SSID: ${u.ssid}`);
    if (u.pendingApply) parts.push('Applying...');
    if (u.rootApplied) parts.push('Root applied');
    if (u.lastError) parts.push(`Error: ${u.lastError}`);
    statusEl.textContent = parts.join(' · ');
  }

  // ---- Mixer UI ----

  function getMixerStream(id) {
    const stream = (mixerState.streams || []).find((s) => s.id === id);
    return stream ? { ...stream } : makeDefaultStream(id);
  }

  function applyMixerToUI(m) {
    if (!m) return;
    const normalized = normalizeMixer(m);
    if (!normalized) return;
    Object.assign(mixerState, normalized, {
      streams: normalized.streams.map((stream) => ({ ...stream }))
    });

    const masterSlider = $('master-out-gain-slider');
    const masterPct = $('master-out-gain-pct');
    if (masterSlider) masterSlider.value = String(mixerState.outGainPct);
    if (masterPct) masterPct.textContent = `${mixerState.outGainPct}%`;

    for (let streamId = 1; streamId <= MIXER_MAX_STREAMS; streamId++) {
      const stream = getMixerStream(streamId);
      const row = document.querySelector(`.stream-row[data-stream-id="${streamId}"]`);
      const slider = $(`stream-${streamId}-gain-slider`);
      const pct = $(`stream-${streamId}-gain-pct`);
      const stateEl = $(`stream-${streamId}-state`);
      const enableBtn = $(`stream-${streamId}-enable-btn`);
      const muteBtn = $(`stream-${streamId}-mute-btn`);
      const soloBtn = $(`stream-${streamId}-solo-btn`);

      if (slider) {
        slider.value = String(stream.gainPct);
        slider.disabled = !stream.enabled;
      }
      if (pct) pct.textContent = `${stream.gainPct}%`;
      if (stateEl) stateEl.textContent = !stream.enabled ? 'DISABLED' : (stream.active ? 'ACTIVE' : 'INACTIVE');

      if (enableBtn) {
        enableBtn.classList.toggle('stream-toggle-btn-active', stream.enabled);
        enableBtn.setAttribute('aria-pressed', String(stream.enabled));
        enableBtn.textContent = stream.enabled ? 'ENABLED' : 'DISABLED';
      }
      if (muteBtn) {
        muteBtn.classList.toggle('mute-btn-active', stream.muted);
        muteBtn.setAttribute('aria-pressed', String(stream.muted));
        muteBtn.disabled = !stream.enabled;
      }
      if (soloBtn) {
        soloBtn.classList.toggle('stream-toggle-btn-active', stream.solo);
        soloBtn.setAttribute('aria-pressed', String(stream.solo));
        soloBtn.disabled = !stream.enabled;
      }
      if (row) {
        row.classList.toggle('stream-row-inactive', !stream.active);
        row.classList.toggle('stream-row-disabled', !stream.enabled);
      }
    }

    // Latency buttons
    document.querySelectorAll('.latency-btn').forEach((btn) => {
      const frames = parseInt(btn.dataset.frames, 10);
      const active = mixerState.jitterFrames === frames || (mixerState.jitterFrames === -1 && frames === JITTER_STABLE_FRAMES);
      btn.classList.toggle('seg-btn-active', active);
    });

    // Legacy input source buttons (if present)
    const auxBtn = $('src-aux-btn');
    const toneBtn = $('src-tone-btn');
    if (auxBtn) auxBtn.classList.toggle('seg-btn-active', mixerState.inputMode !== 'tone');
    if (toneBtn) toneBtn.classList.toggle('seg-btn-active', mixerState.inputMode === 'tone');
  }

  function showMixerRole() {
    const titleEl = $('mixer-pane-title');
    if (titleEl) titleEl.textContent = 'MIXER';
  }

  async function sendMixerPatch(patch) {
    try {
      await portalFetch('/api/mixer', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify(patch)
      });
    } catch (_) {}
  }

  function debouncedMixerSend(patch) {
    Object.assign(mixerState, patch);
    clearTimeout(mixerSendTimer);
    mixerSendTimer = setTimeout(() => sendMixerPatch(patch), 120);
  }

  function updateLocalStream(streamId, patch) {
    mixerState.streams = (mixerState.streams || []).map((stream) => (
      stream.id === streamId ? { ...stream, ...patch } : stream
    ));
  }

  function buildStreamPatch(streamId) {
    const stream = getMixerStream(streamId);
    return {
      schemaVersion: MIXER_SCHEMA_VERSION,
      streams: [{
        id: stream.id,
        gainPct: stream.gainPct,
        enabled: stream.enabled,
        muted: stream.muted,
        solo: stream.solo,
        active: stream.active
      }]
    };
  }

  function bindMixerEvents() {
    const masterSlider = $('master-out-gain-slider');
    const masterPct = $('master-out-gain-pct');
    if (masterSlider) {
      masterSlider.addEventListener('input', () => {
        const gainPct = clampInt(masterSlider.value, MIXER_GAIN_MIN_PCT, MIXER_GAIN_MAX_PCT, mixerState.outGainPct);
        if (masterPct) masterPct.textContent = `${gainPct}%`;
        debouncedMixerSend({ outGainPct: gainPct });
      });
    }

    for (let streamId = 1; streamId <= MIXER_MAX_STREAMS; streamId++) {
      const slider = $(`stream-${streamId}-gain-slider`);
      const pct = $(`stream-${streamId}-gain-pct`);
      const enableBtn = $(`stream-${streamId}-enable-btn`);
      const muteBtn = $(`stream-${streamId}-mute-btn`);
      const soloBtn = $(`stream-${streamId}-solo-btn`);

      if (slider) {
        slider.addEventListener('input', () => {
          const gainPct = clampInt(slider.value, MIXER_GAIN_MIN_PCT, MIXER_GAIN_MAX_PCT, 100);
          if (pct) pct.textContent = `${gainPct}%`;
          updateLocalStream(streamId, { gainPct });
          clearTimeout(mixerSendTimer);
          mixerSendTimer = setTimeout(() => sendMixerPatch(buildStreamPatch(streamId)), 120);
        });
      }

      if (enableBtn) {
        enableBtn.addEventListener('click', () => {
          const stream = getMixerStream(streamId);
          const enabled = !stream.enabled;
          updateLocalStream(streamId, {
            enabled,
            active: enabled ? stream.active : false
          });
          applyMixerToUI(mixerState);
          sendMixerPatch(buildStreamPatch(streamId));
        });
      }

      if (muteBtn) {
        muteBtn.addEventListener('click', () => {
          const stream = getMixerStream(streamId);
          updateLocalStream(streamId, { muted: !stream.muted });
          applyMixerToUI(mixerState);
          sendMixerPatch(buildStreamPatch(streamId));
        });
      }

      if (soloBtn) {
        soloBtn.addEventListener('click', () => {
          const stream = getMixerStream(streamId);
          updateLocalStream(streamId, { solo: !stream.solo });
          applyMixerToUI(mixerState);
          sendMixerPatch(buildStreamPatch(streamId));
        });
      }
    }

    document.querySelectorAll('.latency-btn').forEach((btn) => {
      btn.addEventListener('click', () => {
        const frames = parseInt(btn.dataset.frames, 10);
        mixerState.jitterFrames = frames;
        document.querySelectorAll('.latency-btn').forEach((b) => {
          b.classList.toggle('seg-btn-active', b === btn);
        });
        sendMixerPatch({ jitterFrames: frames });
      });
    });

    document.querySelectorAll('[data-mode]').forEach((btn) => {
      btn.addEventListener('click', () => {
        const mode = btn.dataset.mode;
        if (!mode) return;
        mixerState.inputMode = mode;
        document.querySelectorAll('[data-mode]').forEach((b) => {
          b.classList.toggle('seg-btn-active', b.dataset.mode === mode);
        });
        sendMixerPatch({ inputMode: mode });
      });
    });
  }

  // ---- Mock API ----

  function jsonResponse(status, payload) {
    return {
      ok: status >= 200 && status < 300,
      status,
      async json() { return payload; }
    };
  }

  function buildDemoPayload() {
    const now = Date.now();
    const tick = demoRuntime.tick++;
    const root = 'A3:F2:01:02:03:04';
    const mid = 'B1:C4:05:06:07:08';
    const leaf1 = 'C2:D5:09:0A:0B:0C';
    const leaf2 = 'D3:E6:0D:0E:0F:10';
    const pulse = Math.sin(tick / 8);
    const jitter = (base, amp) => Math.round(base + (amp * pulse));

    const monitor = [
      { seq: tick, line: `I portal: demo tick=${tick}` },
      { seq: tick + 1, line: `I mesh: root=${root} nodes=4 latency=${jitter(12, 3)}ms` }
    ];

    return {
      ts: now,
      self: root,
      heapKb: jitter(92, 4),
      core0LoadPct: Math.max(0, Math.min(100, jitter(38, 12))),
      latencyMs: Math.max(0, jitter(12, 4)),
      netIf: 'usb_ncm (10.48.0.1)',
      buildLabel: 'SRC',
      meshState: 'Mesh OK',
      bpm: null,
      fftBins: Array.from({ length: 28 }, (_, i) => {
        const band = Math.sin((tick / 9) + i * 0.35) * 0.3 + 0.45;
        return Math.max(0, Math.min(1, band));
      }),
      monitor,
      ota: { enabled: true, mode: 'https' },
      uplink: { ...demoRuntime.uplink },
      mixer: {
        ...demoRuntime.mixer,
        streams: (demoRuntime.mixer.streams || []).map((stream) => ({ ...stream }))
      },
      nodes: [
        { mac: root,  role: 'SRC', root: true,  layer: 0, rssi: 0,            children: 2, streaming: true,                 parent: null, uptime: 390000 + tick * 1000, stale: false },
        { mac: mid,   role: 'OUT', root: false, layer: 1, rssi: jitter(-58,4), children: 1, streaming: true,                 parent: root, uptime: 380000 + tick * 1000, stale: false },
        { mac: leaf1, role: 'OUT', root: false, layer: 1, rssi: jitter(-66,4), children: 0, streaming: tick % 12 !== 0,      parent: root, uptime: 365000 + tick * 1000, stale: false },
        { mac: leaf2, role: 'OUT', root: false, layer: 2, rssi: jitter(-74,5), children: 0, streaming: tick % 9 !== 0,       parent: mid,  uptime:  95000 + tick * 1000, stale: false }
      ]
    };
  }

  function ingestPortalPayload(payload) {
    const normalized = normalizePayload(payload);
    Object.assign(state, normalized);
    if (normalized.mixer) {
      applyMixerToUI(normalized.mixer);
    }
    if (selectedMac && !(state.nodes || []).some((n) => n.mac === selectedMac)) {
      selectedMac = null;
    }
    updateAll();
  }

  async function demoFetch(path, options = {}) {
    const method = (options.method || 'GET').toUpperCase();
    const payload = options.body ? JSON.parse(options.body) : {};

    if (path === '/api/status' && method === 'GET') {
      return jsonResponse(200, buildDemoPayload());
    }
    if (path === '/api/uplink' && method === 'GET') {
      return jsonResponse(200, { ...demoRuntime.uplink });
    }
    if (path === '/api/uplink' && method === 'POST') {
      if (typeof payload.enabled !== 'boolean') return jsonResponse(400, { error: 'missing enabled' });
      if (payload.enabled && (!payload.ssid || !String(payload.ssid).trim())) return jsonResponse(400, { error: 'missing ssid' });
      demoRuntime.uplink.enabled = payload.enabled;
      demoRuntime.uplink.configured = payload.enabled;
      demoRuntime.uplink.rootApplied = payload.enabled;
      demoRuntime.uplink.pendingApply = false;
      demoRuntime.uplink.ssid = payload.enabled ? String(payload.ssid).trim() : '';
      demoRuntime.uplink.lastError = '';
      demoRuntime.uplink.updatedMs = Date.now();
      return jsonResponse(200, { ok: true });
    }
    if (path === '/api/mixer' && method === 'GET') {
      return jsonResponse(200, {
        ...demoRuntime.mixer,
        streams: (demoRuntime.mixer.streams || []).map((stream) => ({ ...stream }))
      });
    }
    if (path === '/api/mixer' && method === 'POST') {
      if (Number.isFinite(payload.outGainPct)) {
        demoRuntime.mixer.outGainPct = clampInt(payload.outGainPct, MIXER_GAIN_MIN_PCT, MIXER_GAIN_MAX_PCT, demoRuntime.mixer.outGainPct);
      } else if (Number.isFinite(payload.outputGainDb)) {
        demoRuntime.mixer.outGainPct = pctFromLegacyDb(payload.outputGainDb, demoRuntime.mixer.outGainPct);
      }
      if (Array.isArray(payload.streams)) {
        const nextStreams = demoRuntime.mixer.streams.map((stream) => ({ ...stream }));
        payload.streams.forEach((patchStream) => {
          const id = clampInt(patchStream?.id ?? patchStream?.stream_id, 1, MIXER_MAX_STREAMS, 0);
          if (!id) return;
          const idx = nextStreams.findIndex((stream) => stream.id === id);
          if (idx < 0) return;
          nextStreams[idx] = {
            ...nextStreams[idx],
            id,
            gainPct: clampInt(patchStream.gainPct ?? patchStream.gain_pct, MIXER_GAIN_MIN_PCT, MIXER_GAIN_MAX_PCT, nextStreams[idx].gainPct),
            enabled: typeof patchStream.enabled === 'boolean' ? patchStream.enabled : nextStreams[idx].enabled,
            muted: typeof patchStream.muted === 'boolean' ? patchStream.muted : nextStreams[idx].muted,
            solo: typeof patchStream.solo === 'boolean' ? patchStream.solo : nextStreams[idx].solo,
            active: typeof patchStream.active === 'boolean' ? patchStream.active : nextStreams[idx].active
          };
        });
        demoRuntime.mixer.streams = nextStreams;
        demoRuntime.mixer.streamCount = nextStreams.length;
      }
      return jsonResponse(200, { ok: true });
    }
    if (path === '/api/ota' && method === 'GET') {
      return jsonResponse(200, { enabled: true, inProgress: false, progress: 0, status: 'idle', lastError: '' });
    }
    if (path === '/api/ota' && method === 'POST') {
      if (!payload.url) return jsonResponse(400, { error: 'missing url' });
      if (!String(payload.url).startsWith('https://')) return jsonResponse(409, { error: 'ota start failed' });
      return jsonResponse(200, { ok: true });
    }
    return jsonResponse(404, { error: 'not found' });
  }

  async function portalFetch(path, options) {
    if (demoMode) return demoFetch(path, options);
    return fetch(path, options);
  }

  // ---- Uplink form ----

  async function postUplink(enabled) {
    const ssidEl = $('uplink-ssid');
    const passEl = $('uplink-password');
    const body = enabled
      ? { enabled: true, ssid: (ssidEl?.value || '').trim(), password: passEl?.value || '' }
      : { enabled: false };
    if (enabled && !body.ssid) { window.alert('SSID is required.'); return; }
    const res = await portalFetch('/api/uplink', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(body)
    });
    if (!res.ok) throw new Error(`HTTP ${res.status}`);
  }

  // ---- Monitor output ----

  function updateMonitorOutput() {
    const monitorEl = $('monitor-output');
    const monitorEmpty = $('monitor-empty');
    if (!monitorEl || !monitorEmpty) return;
    const lines = Array.isArray(state.monitor) ? state.monitor : [];
    if (!lines.length) {
      monitorEl.textContent = '';
      monitorEmpty.style.display = 'block';
      return;
    }
    monitorEmpty.style.display = 'none';
    monitorEl.textContent = lines.slice(-40).map((item) => {
      const seq = typeof item?.seq === 'number' ? String(item.seq).padStart(5, '0') : '-----';
      const line = typeof item?.line === 'string' ? item.line : '';
      return `[${seq}] ${line}`;
    }).join('\n');
    monitorEl.scrollTop = monitorEl.scrollHeight;
  }

  // ---- Global readouts ----

  function updateGlobalReadouts() {
    const nodes = state.nodes || [];
    const root = nodes.find((n) => n.root) || nodes[0] || null;
    const streaming = nodes.filter((n) => n.streaming).length;
    const stale = nodes.filter((n) => n.stale).length;

    $('stat-nodes').textContent = String(nodes.length);
    $('stat-streaming').textContent = `${streaming}/${nodes.length || 0}`;
    $('stat-health').textContent = state.meshState || (stale === 0 ? 'Mesh OK' : `${stale} stale`);

    $('footer-mesh-nodes').textContent = String(nodes.length);
    $('footer-state').textContent = state.meshState || (stale === 0 ? 'Mesh OK' : 'Mesh Degraded');
    $('footer-netif').textContent = state.netIf || 'usb_ncm (10.48.0.1)';
    $('build-label').textContent = state.buildLabel ? `${state.buildLabel} v1.0.1` : 'SRC v1.0.1';

    const uptime = root ? root.uptime : 0;
    const uptimeEl = $('uptime-value');
    if (uptimeEl) uptimeEl.textContent = formatUptime(uptime);

    $('core0-load').textContent = numOrNA(state.core0LoadPct);
    $('heap-free').textContent = numOrNA(state.heapKb);

    showMixerRole();
  }

  // ---- OTA ----

  async function triggerOtaFromPrompt() {
    const url = window.prompt('Enter HTTPS firmware URL for OTA update:');
    if (!url) return;
    try {
      const res = await portalFetch('/api/ota', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ url: url.trim() })
      });
      if (!res.ok) throw new Error(`HTTP ${res.status}`);
      window.alert('OTA accepted. Device will download and reboot when complete.');
    } catch (err) {
      window.alert(`OTA request failed: ${err.message || err}`);
    }
  }

  // ---- Selected node detail ----

  function renderSelectedNode() {
    const box = $('selected-node-details');
    const empty = $('selected-node-empty');
    const node = (state.nodes || []).find((n) => n.mac === selectedMac);

    if (!node) {
      box.innerHTML = '';
      empty.style.display = 'block';
      return;
    }

    empty.style.display = 'none';
    box.innerHTML = [
      ['MAC', node.mac],
      ['Role', node.role],
      ['Layer', String(node.layer)],
      ['RSSI', `${node.rssi} dBm`],
      ['Children', String(node.children)],
      ['Streaming', node.streaming ? 'Yes' : 'No'],
      ['Uptime', formatUptime(node.uptime)],
      ['Parent', node.parent || '— (root)']
    ].map(([k, v]) => `<div class="row"><span>${k}</span><span>${v}</span></div>`).join('');
  }

  // ---- Canvas helpers ----

  function resizeCanvas(canvas, ctx) {
    const rect = canvas.getBoundingClientRect();
    canvas.width = Math.max(1, Math.floor(rect.width * dpr));
    canvas.height = Math.max(1, Math.floor(rect.height * dpr));
    ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
  }

  // ---- Topology layout & drawing ----

  function getLayerLayout(rect, nodes) {
    const layers = {};
    nodes.forEach((n) => {
      const layer = n.layer || 0;
      if (!layers[layer]) layers[layer] = [];
      layers[layer].push(n);
    });
    Object.keys(layers).forEach((k) => { layers[k].sort((a, b) => a.mac.localeCompare(b.mac)); });
    const keys = Object.keys(layers).map(Number).sort((a, b) => a - b);
    const maxR = Math.min(rect.width, rect.height) * 0.38;
    const radiiByLayer = {};
    keys.forEach((layer, idx) => {
      radiiByLayer[layer] = layer === 0 ? 0 : (idx / Math.max(1, keys.length - 1)) * maxR;
    });
    return { layers, keys, radiiByLayer, centerX: rect.width / 2, centerY: rect.height / 2 };
  }

  function computeLayout() {
    nodePositions = {};
    const nodes = state.nodes || [];
    if (!nodes.length) return;
    const rect = topologyCanvas.getBoundingClientRect();
    const layout = getLayerLayout(rect, nodes);
    layout.keys.forEach((layer) => {
      const group = layout.layers[layer];
      if (!group || !group.length) return;
      const radius = layout.radiiByLayer[layer];
      group.forEach((node, i) => {
        const a = group.length === 1 ? -Math.PI / 2 : (2 * Math.PI * i) / group.length - Math.PI / 2;
        nodePositions[node.mac] = {
          x: layout.centerX + radius * Math.cos(a),
          y: layout.centerY + radius * Math.sin(a)
        };
      });
    });
  }

  function edgeColor(rssi) {
    if (rssi > -60) return '#76ff03';
    if (rssi >= -75) return '#ffd166';
    return '#ff5d73';
  }

  function drawTopology(t) {
    const rect = topologyCanvas.getBoundingClientRect();
    topologyCtx.clearRect(0, 0, rect.width, rect.height);
    const nodes = state.nodes || [];
    const layout = getLayerLayout(rect, nodes);
    const rootNode = nodes.find((n) => n.root && nodePositions[n.mac]) || nodes.find((n) => n.layer === 0 && nodePositions[n.mac]);
    const ringCx = rootNode ? nodePositions[rootNode.mac].x : layout.centerX;
    const ringCy = rootNode ? nodePositions[rootNode.mac].y : layout.centerY;

    layout.keys.filter((layer) => layer > 0 && layout.radiiByLayer[layer] > 0).forEach((layer) => {
      topologyCtx.beginPath();
      topologyCtx.arc(ringCx, ringCy, layout.radiiByLayer[layer], 0, Math.PI * 2);
      topologyCtx.strokeStyle = 'rgba(118,255,3,0.22)';
      topologyCtx.lineWidth = 1;
      topologyCtx.setLineDash(layer % 2 === 0 ? [4, 5] : []);
      topologyCtx.stroke();
    });
    topologyCtx.setLineDash([]);

    nodes.forEach((n) => {
      if (!n.parent || !nodePositions[n.parent] || !nodePositions[n.mac]) return;
      const from = nodePositions[n.parent];
      const to = nodePositions[n.mac];
      topologyCtx.beginPath();
      topologyCtx.moveTo(from.x, from.y);
      topologyCtx.lineTo(to.x, to.y);
      topologyCtx.strokeStyle = edgeColor(n.rssi);
      topologyCtx.lineWidth = n.stale ? 1 : 2;
      if (n.stale) topologyCtx.setLineDash([4, 4]);
      topologyCtx.stroke();
      topologyCtx.setLineDash([]);

      if (n.streaming) {
        for (let p = 0; p < 3; p++) {
          const frac = (t * 0.45 + p / 3) % 1;
          const x = from.x + (to.x - from.x) * frac;
          const y = from.y + (to.y - from.y) * frac;
          topologyCtx.beginPath();
          topologyCtx.arc(x, y, 2.6, 0, Math.PI * 2);
          topologyCtx.fillStyle = '#76ff03';
          topologyCtx.fill();
        }
      }
    });

    nodes.forEach((n) => {
      const pos = nodePositions[n.mac];
      if (!pos) return;
      const r = n.root ? 16 : 12;
      const col = n.root ? '#2196f3' : '#76ff03';
      topologyCtx.globalAlpha = n.stale ? 0.45 : 1;

      if (n.mac === state.self) {
        topologyCtx.beginPath();
        topologyCtx.arc(pos.x, pos.y, r + 7 + Math.sin(t * 3), 0, Math.PI * 2);
        topologyCtx.strokeStyle = '#76ff03';
        topologyCtx.lineWidth = 1.2;
        topologyCtx.stroke();
      }

      topologyCtx.beginPath();
      topologyCtx.arc(pos.x, pos.y, r, 0, Math.PI * 2);
      topologyCtx.fillStyle = col;
      topologyCtx.fill();

      topologyCtx.fillStyle = '#121212';
      topologyCtx.font = '700 10px "IBM Plex Mono", monospace';
      topologyCtx.textAlign = 'center';
      topologyCtx.textBaseline = 'middle';
      topologyCtx.fillText(n.role || 'RX', pos.x, pos.y);

      topologyCtx.fillStyle = '#e9ecef';
      topologyCtx.font = '10px "IBM Plex Mono", monospace';
      topologyCtx.textBaseline = 'top';
      topologyCtx.fillText(shortMac(n.mac), pos.x, pos.y + r + 4);
      topologyCtx.globalAlpha = 1;
    });
  }

  // ---- FFT drawing ----

  function drawFft(t) {
    if (!fftCanvas || !fftCtx) return;
    const rect = fftCanvas.getBoundingClientRect();
    if (!rect.width || !rect.height) return;
    const w = rect.width;
    const h = rect.height;
    fftCtx.clearRect(0, 0, w, h);

    const bars = 28;
    const pad = 10;
    const innerW = w - pad * 2;
    const innerH = h - pad * 2;
    const barW = innerW / bars;

    const freqs = [20, 50, 100, 200, 500, 1000, 2000, 5000, 10000, 20000];
    fftCtx.strokeStyle = 'rgba(118,255,3,0.22)';
    fftCtx.lineWidth = 1;
    freqs.forEach((f) => {
      const x = pad + (Math.log10(f) - Math.log10(20)) / (Math.log10(20000) - Math.log10(20)) * innerW;
      fftCtx.beginPath();
      fftCtx.moveTo(x, pad);
      fftCtx.lineTo(x, h - pad);
      fftCtx.stroke();
    });

    const fftBins = Array.isArray(state.fftBins) ? state.fftBins : null;
    for (let i = 0; i < bars; i++) {
      const amp = fftBins && typeof fftBins[i] === 'number'
        ? Math.max(0, Math.min(1, fftBins[i])) : 0;
      const barH = innerH * amp;
      fftCtx.fillStyle = '#76ff03';
      fftCtx.fillRect(pad + i * barW + 1, h - pad - barH, Math.max(1, barW - 2), barH);
    }

    if (!fftBins) {
      fftCtx.fillStyle = 'rgba(233,236,239,0.68)';
      fftCtx.font = '10px "IBM Plex Mono", monospace';
      fftCtx.textAlign = 'right';
      fftCtx.textBaseline = 'top';
      fftCtx.fillText('FFT unavailable', w - pad, pad);
    }
  }

  // ---- Topology interaction ----

  function findNodeAt(x, y) {
    const nodes = state.nodes || [];
    for (let i = nodes.length - 1; i >= 0; i--) {
      const n = nodes[i];
      const p = nodePositions[n.mac];
      if (!p) continue;
      const r = n.root ? 16 : 12;
      const dx = x - p.x;
      const dy = y - p.y;
      if (dx * dx + dy * dy <= (r + 6) * (r + 6)) return n;
    }
    return null;
  }

  topologyCanvas.addEventListener('click', (ev) => {
    const rect = topologyCanvas.getBoundingClientRect();
    const node = findNodeAt(ev.clientX - rect.left, ev.clientY - rect.top);
    selectedMac = node ? node.mac : null;
    renderSelectedNode();
  });

  let draggingNode = null;

  topologyCanvas.addEventListener('mousedown', (ev) => {
    const rect = topologyCanvas.getBoundingClientRect();
    const x = ev.clientX - rect.left;
    const y = ev.clientY - rect.top;
    draggingNode = findNodeAt(x, y);
  });

  window.addEventListener('mousemove', (ev) => {
    if (draggingNode) {
      const rect = topologyCanvas.getBoundingClientRect();
      const x = (ev.clientX - rect.left) / rect.width * 200 - 100; // Normalizing to -100 to 100
      const y = (ev.clientY - rect.top) / rect.height * 200 - 100;
      nodePositions[draggingNode.mac] = { x: ev.clientX - rect.left, y: ev.clientY - rect.top };
      draggingNode.x = x;
      draggingNode.y = y;
    }
  });

  window.addEventListener('mouseup', () => {
    draggingNode = null;
  });

  $('update-positions-btn')?.addEventListener('click', async () => {
    const positions = state.nodes.map(n => ({ mac: n.mac, x: n.x || 0, y: n.y || 0, z: n.z || 0 }));
    try {
      await portalFetch('/api/mesh/positions', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ positions })
      });
      window.alert('Layout saved.');
    } catch (err) {
      window.alert(`Failed to save layout: ${err.message}`);
    }
  });

  $('uplink-apply')?.addEventListener('click', async () => {
    try { await postUplink(true); } catch (err) { window.alert(`Failed to apply uplink: ${err.message || err}`); }
  });

  $('uplink-clear')?.addEventListener('click', async () => {
    try { await postUplink(false); } catch (err) { window.alert(`Failed to clear uplink: ${err.message || err}`); }
  });

  // ---- Aggregate update ----

  function updateAll() {
    updateGlobalReadouts();
    updateMonitorOutput();
    updateUplinkStatus();
    computeLayout();
    renderSelectedNode();
  }

  // ---- WebSocket / connection ----

  function scheduleReconnect() {
    wsAttempts += 1;
    setConnState('reconnecting', 'Reconnecting');
    if (wsAttempts >= 3 && !demoMode) {
      demoMode = true;
      startDemo();
      return;
    }
    const delay = Math.min(2000 * Math.pow(2, wsAttempts - 1), 30000);
    clearTimeout(reconnectTimer);
    reconnectTimer = setTimeout(connectWs, delay);
  }

  function connectWs() {
    if (forceDemoMode) {
      if (!demoMode) { demoMode = true; startDemo(); }
      return;
    }
    if (ws && (ws.readyState === 0 || ws.readyState === 1)) return;
    try {
      ws = new WebSocket(`ws://${window.location.host}/ws`);
    } catch (_) {
      scheduleReconnect();
      return;
    }

    ws.onopen = () => {
      wsAttempts = 0;
      demoMode = false;
      if (demoRuntime.intervalId) { clearInterval(demoRuntime.intervalId); demoRuntime.intervalId = null; }
      setConnState('connected', 'Connected');
    };

    ws.onmessage = (ev) => {
      try {
        const msg = JSON.parse(ev.data);
        if (msg && Array.isArray(msg.nodes)) {
          ingestPortalPayload(msg);
        }
      } catch (_) {}
    };

    ws.onclose = () => scheduleReconnect();
    ws.onerror = () => { try { ws.close(); } catch (_) {} };
  }

  function startDemo() {
    setConnState('reconnecting', 'Demo Mode');
    if (demoRuntime.intervalId) { clearInterval(demoRuntime.intervalId); demoRuntime.intervalId = null; }
    ingestPortalPayload(buildDemoPayload());
    demoRuntime.intervalId = setInterval(() => { ingestPortalPayload(buildDemoPayload()); }, 1000);
  }

  // ---- Animation loop ----

  function frame(now) {
    requestAnimationFrame(frame);
    const t = now / 1000;
    if (Math.abs(t - animTime) < 1 / 30) return;
    animTime = t;
    resizeCanvas(topologyCanvas, topologyCtx);
    if (fftCanvas && fftCtx) resizeCanvas(fftCanvas, fftCtx);
    drawTopology(t);
    drawFft(t);
  }

  window.addEventListener('resize', () => {
    resizeCanvas(topologyCanvas, topologyCtx);
    if (fftCanvas && fftCtx) resizeCanvas(fftCanvas, fftCtx);
    updateAll();
  });

  // ---- Init ----

  resizeCanvas(topologyCanvas, topologyCtx);
  if (fftCanvas && fftCtx) resizeCanvas(fftCanvas, fftCtx);
  bindMixerEvents();
  connectWs();
  requestAnimationFrame(frame);
})();
