(function () {
  'use strict';

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
    fftBins: null
  };
  let ws = null;
  let wsAttempts = 0;
  let reconnectTimer = null;
  let demoMode = false;
  let selectedMac = null;
  const forceDemoMode = document.body?.dataset?.forceDemo === '1' ||
    new URLSearchParams(window.location.search).get('demo') === '1';

  const topologyCanvas = document.getElementById('topology-canvas');
  const topologyCtx = topologyCanvas.getContext('2d');
  const fftCanvas = document.getElementById('fft-canvas');
  const fftCtx = fftCanvas.getContext('2d');

  const $ = (id) => document.getElementById(id);
  const connDot = $('conn-dot');
  const connLabel = $('conn-label');
  const wsDot = $('ws-dot');
  const wsLabel = $('ws-label');

  const dpr = window.devicePixelRatio || 1;
  let nodePositions = {};
  let animTime = 0;

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
    // Map TX→SRC, RX→OUT for display
    let rawRole = typeof rawNode.role === 'string' ? rawNode.role : 'RX';
    let displayRole = rawRole === 'TX' ? 'SRC' : (rawRole === 'RX' ? 'OUT' : rawRole);
    return {
      mac,
      role: displayRole,
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
      fftBins: Array.isArray(msg?.fftBins) ? msg.fftBins : null
    };
  }

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
    $('build-label').textContent = state.buildLabel ? `${state.buildLabel} v1.0.1` : 'TX v1.0.1';

    const uptime = root ? root.uptime : 0;
    $('uptime-value').textContent = formatUptime(uptime);

    $('bpm-value').textContent = state.bpm !== null ? numOrNA(state.bpm) : 'No data';

    $('core0-load').textContent = numOrNA(state.core0LoadPct);
    $('heap-free').textContent = numOrNA(state.heapKb);
  }

  function rssiToBars(rssi) {
    // Map RSSI to 0-5 bars: >-50=5, >-60=4, >-70=3, >-80=2, >-90=1, else=0
    if (rssi > -50) return 5;
    if (rssi > -60) return 4;
    if (rssi > -70) return 3;
    if (rssi > -80) return 2;
    if (rssi > -90) return 1;
    return 0;
  }

  function renderRssiBars(rssi) {
    const bars = rssiToBars(rssi);
    const filled = '█'.repeat(bars);
    const empty = '░'.repeat(5 - bars);
    return `[${filled}${empty}] ${rssi} dBm`;
  }

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
    const streamLabel = node.streaming ? 'Yes' : 'No';
    const roleLabel = node.root ? `${node.role} (Root)` : node.role;
    box.innerHTML = [
      ['MAC', node.mac],
      ['Role', roleLabel],
      ['Layer', String(node.layer)],
      ['RSSI', renderRssiBars(node.rssi)],
      ['Children', String(node.children)],
      ['Streaming', streamLabel],
      ['Uptime', formatUptime(node.uptime)],
      ['Parent', node.parent ? shortMac(node.parent) : '— (root)']
    ].map(([k, v]) => `<div class="row"><span>${k}</span><span>${v}</span></div>`).join('');
  }

  function resizeCanvas(canvas, ctx) {
    const rect = canvas.getBoundingClientRect();
    canvas.width = Math.max(1, Math.floor(rect.width * dpr));
    canvas.height = Math.max(1, Math.floor(rect.height * dpr));
    ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
  }

  function getLayerLayout(rect, nodes) {
    const layers = {};
    nodes.forEach((n) => {
      const layer = n.layer || 0;
      if (!layers[layer]) layers[layer] = [];
      layers[layer].push(n);
    });

    Object.keys(layers).forEach((k) => {
      layers[k].sort((a, b) => a.mac.localeCompare(b.mac));
    });

    const keys = Object.keys(layers).map(Number).sort((a, b) => a - b);
    const maxR = Math.min(rect.width, rect.height) * 0.40;
    const minR = 70; // Minimum radius for layer 1 to separate from root
    const radiiByLayer = {};
    keys.forEach((layer) => {
      if (layer === 0) {
        radiiByLayer[layer] = 0; // Root at center
      } else {
        // Spread layers evenly between minR and maxR
        const maxLayer = Math.max(...keys.filter(k => k > 0), 1);
        radiiByLayer[layer] = minR + ((layer - 1) / Math.max(1, maxLayer - 1)) * (maxR - minR);
        if (maxLayer === 1) radiiByLayer[layer] = minR; // Only one non-root layer
      }
    });

    return {
      layers,
      keys,
      radiiByLayer,
      centerX: rect.width / 2,
      centerY: rect.height / 2
    };
  }

  function computeLayout() {
    nodePositions = {};
    const nodes = state.nodes || [];
    if (!nodes.length) return;

    const rect = topologyCanvas.getBoundingClientRect();
    const layout = getLayerLayout(rect, nodes);
    const layers = layout.layers;
    const keys = layout.keys;
    keys.forEach((layer, idx) => {
      const group = layers[layer];
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

    layout.keys
      .filter((layer) => layer > 0 && layout.radiiByLayer[layer] > 0)
      .forEach((layer) => {
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

    // Draw a star shape for root node
    function drawStar(cx, cy, spikes, outerR, innerR) {
      topologyCtx.beginPath();
      for (let i = 0; i < spikes * 2; i++) {
        const r = i % 2 === 0 ? outerR : innerR;
        const angle = (Math.PI / spikes) * i - Math.PI / 2;
        const x = cx + r * Math.cos(angle);
        const y = cy + r * Math.sin(angle);
        if (i === 0) topologyCtx.moveTo(x, y);
        else topologyCtx.lineTo(x, y);
      }
      topologyCtx.closePath();
    }

    nodes.forEach((n) => {
      const pos = nodePositions[n.mac];
      if (!pos) return;
      const r = n.root ? 28 : 14;
      const col = n.root ? '#2196f3' : '#76ff03';
      topologyCtx.globalAlpha = n.stale ? 0.45 : 1;

      // Pulsing ring for self node
      if (n.mac === state.self) {
        topologyCtx.beginPath();
        topologyCtx.arc(pos.x, pos.y, r + 8 + Math.sin(t * 3) * 2, 0, Math.PI * 2);
        topologyCtx.strokeStyle = 'rgba(255,255,255,0.5)';
        topologyCtx.lineWidth = 2;
        topologyCtx.stroke();
      }

      // Draw node circle
      topologyCtx.beginPath();
      topologyCtx.arc(pos.x, pos.y, r, 0, Math.PI * 2);
      topologyCtx.fillStyle = col;
      topologyCtx.fill();

      // Draw star icon for root, text label for others
      if (n.root) {
        topologyCtx.fillStyle = '#121212';
        drawStar(pos.x, pos.y, 5, 14, 6);
        topologyCtx.fill();
      } else {
        topologyCtx.fillStyle = '#121212';
        topologyCtx.font = '700 10px "IBM Plex Mono", monospace';
        topologyCtx.textAlign = 'center';
        topologyCtx.textBaseline = 'middle';
        topologyCtx.fillText(n.role || 'OUT', pos.x, pos.y);
      }

      // MAC label below
      topologyCtx.fillStyle = '#e9ecef';
      topologyCtx.font = '10px "IBM Plex Mono", monospace';
      topologyCtx.textAlign = 'center';
      topologyCtx.textBaseline = 'top';
      topologyCtx.fillText(shortMac(n.mac), pos.x, pos.y + r + 4);

      // Role label for root
      if (n.root) {
        topologyCtx.fillText('ROOT ' + (n.role || 'SRC'), pos.x, pos.y + r + 16);
      }
      topologyCtx.globalAlpha = 1;
    });
  }

  function drawFft(t) {
    const rect = fftCanvas.getBoundingClientRect();
    const w = rect.width;
    const h = rect.height;
    fftCtx.clearRect(0, 0, w, h);

    const bars = 28;
    const pad = 10;
    const innerW = w - pad * 2;
    const innerH = h - pad * 2;
    const barW = innerW / bars;

    // Log-frequency grid lines.
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
        ? Math.max(0, Math.min(1, fftBins[i]))
        : 0;
      const barH = innerH * amp;
      fftCtx.fillStyle = '#76ff03';
      fftCtx.fillRect(pad + i * barW + 1, h - pad - barH, Math.max(1, barW - 2), barH);
    }

    if (!fftBins) {
      fftCtx.fillStyle = 'rgba(233,236,239,0.68)';
      fftCtx.font = '10px "IBM Plex Mono", monospace';
      fftCtx.textAlign = 'right';
      fftCtx.textBaseline = 'top';
      fftCtx.fillText('FFT telemetry unavailable', w - pad, pad);
    }
  }

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

  function updateAll() {
    updateGlobalReadouts();
    computeLayout();
    renderSelectedNode();
  }

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
      if (!demoMode) {
        demoMode = true;
        startDemo();
      }
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
      setConnState('connected', 'Connected');
    };

    ws.onmessage = (ev) => {
      try {
        const msg = JSON.parse(ev.data);
        if (msg && Array.isArray(msg.nodes)) {
          Object.assign(state, normalizePayload(msg));
          if (selectedMac && !(state.nodes || []).some((n) => n.mac === selectedMac)) {
            selectedMac = null;
          }
          updateAll();
        }
      } catch (_) {}
    };

    ws.onclose = () => scheduleReconnect();
    ws.onerror = () => {
      try { ws.close(); } catch (_) {}
    };
  }

  function startDemo() {
    setConnState('reconnecting', 'Demo Mode');
    const root = 'A3:F2:01:02:03:04';
    state.self = root;
    state.heapKb = 92;
    state.core0LoadPct = null;
    state.latencyMs = 12;
    state.buildLabel = 'TX';
    state.meshState = 'Mesh OK';
    state.bpm = null;
    state.fftBins = null;
    state.nodes = [
      { mac: root, role: 'SRC', root: true, layer: 0, rssi: 0, children: 2, streaming: true, parent: null, uptime: 390000, stale: false },
      { mac: 'B1:C4:05:06:07:08', role: 'OUT', root: false, layer: 1, rssi: -58, children: 1, streaming: true, parent: root, uptime: 380000, stale: false },
      { mac: 'C2:D5:09:0A:0B:0C', role: 'OUT', root: false, layer: 1, rssi: -66, children: 0, streaming: true, parent: root, uptime: 365000, stale: false },
      { mac: 'D3:E6:0D:0E:0F:10', role: 'OUT', root: false, layer: 2, rssi: -74, children: 0, streaming: true, parent: 'B1:C4:05:06:07:08', uptime: 95000, stale: false }
    ];

    setInterval(() => {
      state.nodes.forEach((n, i) => {
        n.uptime += 1000;
        if (i > 0) n.rssi = Math.max(-90, Math.min(-45, n.rssi + Math.round((Math.random() - 0.5) * 4)));
      });
      if (Math.random() < 0.12) {
        const n = state.nodes[state.nodes.length - 1];
        n.streaming = !n.streaming;
        n.stale = false;
      }
      updateAll();
    }, 1000);

    updateAll();
  }

  function frame(now) {
    requestAnimationFrame(frame);
    const t = now / 1000;
    if (Math.abs(t - animTime) < 1 / 30) return;
    animTime = t;
    resizeCanvas(topologyCanvas, topologyCtx);
    resizeCanvas(fftCanvas, fftCtx);
    drawTopology(t);
    drawFft(t);
  }

  window.addEventListener('resize', () => {
    resizeCanvas(topologyCanvas, topologyCtx);
    resizeCanvas(fftCanvas, fftCtx);
    updateAll();
  });

  resizeCanvas(topologyCanvas, topologyCtx);
  resizeCanvas(fftCanvas, fftCtx);
  connectWs();
  requestAnimationFrame(frame);
})();
