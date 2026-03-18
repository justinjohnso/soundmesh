(function () {
  'use strict';

  const state = { ts: 0, self: '', nodes: [] };
  let ws = null;
  let wsAttempts = 0;
  let reconnectTimer = null;
  let demoMode = false;
  let selectedMac = null;

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

  function updateGlobalReadouts() {
    const nodes = state.nodes || [];
    const root = nodes.find((n) => n.root) || nodes[0] || null;
    const streaming = nodes.filter((n) => n.streaming).length;
    const stale = nodes.filter((n) => n.stale).length;

    $('stat-nodes').textContent = String(nodes.length);
    $('stat-streaming').textContent = `${streaming}/${nodes.length || 0}`;
    $('stat-health').textContent = stale === 0 ? 'Mesh OK' : `${stale} stale`;

    $('footer-mesh-nodes').textContent = String(nodes.length);
    $('footer-state').textContent = stale === 0 ? 'Mesh OK' : 'Mesh Degraded';

    const uptime = root ? root.uptime : 0;
    $('uptime-value').textContent = formatUptime(uptime);

    const bpm = 126 + Math.round(Math.sin(Date.now() / 1200) * Math.min(4, Math.max(1, streaming)));
    $('bpm-value').textContent = String(bpm);

    if (typeof state.core0Load === 'number') $('core0-load').textContent = String(state.core0Load);
    if (typeof state.heapKb === 'number') $('heap-free').textContent = String(state.heapKb);
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

  function resizeCanvas(canvas, ctx) {
    const rect = canvas.getBoundingClientRect();
    canvas.width = Math.max(1, Math.floor(rect.width * dpr));
    canvas.height = Math.max(1, Math.floor(rect.height * dpr));
    ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
  }

  function computeLayout() {
    nodePositions = {};
    const nodes = state.nodes || [];
    if (!nodes.length) return;

    const rect = topologyCanvas.getBoundingClientRect();
    const cx = rect.width / 2;
    const cy = rect.height / 2;
    const maxR = Math.min(rect.width, rect.height) * 0.38;

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
    keys.forEach((layer, idx) => {
      const group = layers[layer];
      if (!group || !group.length) return;
      const radius = layer === 0 ? 0 : (idx / Math.max(1, keys.length - 1)) * maxR;
      group.forEach((node, i) => {
        const a = group.length === 1 ? -Math.PI / 2 : (2 * Math.PI * i) / group.length - Math.PI / 2;
        nodePositions[node.mac] = {
          x: cx + radius * Math.cos(a),
          y: cy + radius * Math.sin(a)
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

    const streamFactor = Math.max(1, (state.nodes || []).filter((n) => n.streaming).length);
    for (let i = 0; i < bars; i++) {
      const norm = i / (bars - 1);
      const spectralPeak = Math.exp(-Math.pow((norm - 0.38) * 4.0, 2));
      const wobble = (Math.sin(t * 6 + i * 0.55) + 1) * 0.22;
      const noise = (Math.sin(t * 2.4 + i * 1.7) + 1) * 0.05;
      const amp = Math.min(0.98, (spectralPeak * 0.68 + wobble + noise) * (0.82 + streamFactor * 0.06));
      const barH = innerH * amp;
      fftCtx.fillStyle = '#76ff03';
      fftCtx.fillRect(pad + i * barW + 1, h - pad - barH, Math.max(1, barW - 2), barH);
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
          Object.assign(state, msg);
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
    state.nodes = [
      { mac: root, role: 'TX', root: true, layer: 0, rssi: 0, children: 2, streaming: true, parent: null, uptime: 390000, stale: false },
      { mac: 'B1:C4:05:06:07:08', role: 'RX', root: false, layer: 1, rssi: -58, children: 1, streaming: true, parent: root, uptime: 380000, stale: false },
      { mac: 'C2:D5:09:0A:0B:0C', role: 'RX', root: false, layer: 1, rssi: -66, children: 0, streaming: true, parent: root, uptime: 365000, stale: false },
      { mac: 'D3:E6:0D:0E:0F:10', role: 'RX', root: false, layer: 2, rssi: -74, children: 0, streaming: false, parent: 'B1:C4:05:06:07:08', uptime: 95000, stale: true }
    ];

    setInterval(() => {
      state.nodes.forEach((n, i) => {
        n.uptime += 1000;
        if (i > 0) n.rssi = Math.max(-90, Math.min(-45, n.rssi + Math.round((Math.random() - 0.5) * 4)));
      });
      if (Math.random() < 0.12) {
        const n = state.nodes[state.nodes.length - 1];
        n.streaming = !n.streaming;
        n.stale = !n.streaming;
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
