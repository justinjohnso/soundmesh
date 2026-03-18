(function () {
  "use strict";

  // --- State ---
  var state = { ts: 0, self: "", nodes: [] };
  var selectedMac = null;
  var wsConnected = false;
  var demoMode = false;
  var wsAttempts = 0;
  var ws = null;
  var reconnectTimer = null;
  var animT = 0;
  var lastFrame = 0;

  // --- DOM refs ---
  var canvas = document.getElementById("canvas");
  var ctx = canvas.getContext("2d");
  var connDot = document.getElementById("conn-dot");
  var connLabel = document.getElementById("conn-label");
  var statNodes = document.getElementById("stat-nodes");
  var statStreaming = document.getElementById("stat-streaming");
  var statHealth = document.getElementById("stat-health");
  var detailPanel = document.getElementById("detail-panel");
  var panelBody = document.getElementById("panel-body");
  var panelClose = document.getElementById("panel-close");

  // --- Node layout cache ---
  var nodePositions = {}; // mac -> {x, y}

  // --- Color constants ---
  var COL_TX = "#0066ff";
  var COL_RX = "#00cc66";
  var COL_COMBO = "#9933ff";
  var COL_CYAN = "#00d9ff";
  var COL_EDGE_GOOD = "#00d9ff";
  var COL_EDGE_OK = "#ffd700";
  var COL_EDGE_BAD = "#ff4444";

  // =========================================================
  // WebSocket
  // =========================================================
  function wsConnect() {
    if (ws && (ws.readyState === 0 || ws.readyState === 1)) return;
    try {
      ws = new WebSocket("ws://" + window.location.host + "/ws");
    } catch (e) {
      scheduleReconnect();
      return;
    }
    ws.onopen = function () {
      wsConnected = true;
      wsAttempts = 0;
      demoMode = false;
      setConnStatus("connected");
    };
    ws.onmessage = function (ev) {
      try {
        var msg = JSON.parse(ev.data);
        if (msg && msg.nodes) {
          state = msg;
          updateStatusBar();
          if (selectedMac) updateDetailPanel(selectedMac);
        }
      } catch (_) {}
    };
    ws.onclose = function () {
      wsConnected = false;
      setConnStatus("disconnected");
      scheduleReconnect();
    };
    ws.onerror = function () {
      wsConnected = false;
      ws.close();
    };
  }

  function scheduleReconnect() {
    wsAttempts++;
    if (wsAttempts >= 3 && !demoMode) {
      demoMode = true;
      setConnStatus("demo");
      startDemo();
      return;
    }
    var delay = Math.min(2000 * Math.pow(2, wsAttempts - 1), 30000);
    clearTimeout(reconnectTimer);
    reconnectTimer = setTimeout(wsConnect, delay);
  }

  function setConnStatus(s) {
    if (s === "connected") {
      connDot.style.background = "#00d9ff";
      connLabel.textContent = "Connected";
    } else if (s === "demo") {
      connDot.style.background = "#ffd700";
      connLabel.textContent = "Demo Mode";
    } else {
      connDot.style.background = "#ff4444";
      connLabel.textContent = "Disconnected";
    }
  }

  // =========================================================
  // Demo data
  // =========================================================
  var demoTimer = null;

  function startDemo() {
    var macs = [
      "A3:F2:01:02:03:04",
      "B1:C4:05:06:07:08",
      "C2:D5:09:0A:0B:0C",
      "D3:E6:0D:0E:0F:10",
      "E4:F7:11:12:13:14"
    ];
    state = {
      ts: Date.now(),
      self: macs[0],
      nodes: [
        { mac: macs[0], role: "TX", root: true, layer: 0, rssi: 0, children: 2, streaming: true, parent: null, uptime: 384512, stale: false },
        { mac: macs[1], role: "RX", root: false, layer: 1, rssi: -58, children: 1, streaming: true, parent: macs[0], uptime: 382100, stale: false },
        { mac: macs[2], role: "RX", root: false, layer: 1, rssi: -67, children: 0, streaming: true, parent: macs[0], uptime: 380200, stale: false },
        { mac: macs[3], role: "COMBO", root: false, layer: 2, rssi: -72, children: 0, streaming: true, parent: macs[1], uptime: 310000, stale: false },
        { mac: macs[4], role: "RX", root: false, layer: 2, rssi: -81, children: 0, streaming: false, parent: macs[1], uptime: 50000, stale: true }
      ]
    };
    updateStatusBar();

    demoTimer = setInterval(function () {
      for (var i = 1; i < state.nodes.length; i++) {
        var n = state.nodes[i];
        n.rssi += Math.round((Math.random() - 0.5) * 6);
        n.rssi = Math.max(-90, Math.min(-40, n.rssi));
        n.uptime += 1000;
      }
      state.nodes[0].uptime += 1000;
      // Occasionally toggle streaming on last node
      if (Math.random() < 0.1) {
        var last = state.nodes[state.nodes.length - 1];
        last.streaming = !last.streaming;
        last.stale = !last.streaming;
      }
      state.ts = Date.now();
      updateStatusBar();
      if (selectedMac) updateDetailPanel(selectedMac);
    }, 1000);
  }

  // =========================================================
  // Status bar
  // =========================================================
  function updateStatusBar() {
    var n = state.nodes.length;
    var streaming = 0;
    var staleCount = 0;
    for (var i = 0; i < n; i++) {
      if (state.nodes[i].streaming) streaming++;
      if (state.nodes[i].stale) staleCount++;
    }
    statNodes.textContent = "Nodes: " + n;
    statStreaming.textContent = "Streaming: " + streaming + "/" + n;
    var health = staleCount === 0 ? "Healthy" : staleCount + " stale";
    statHealth.textContent = "Mesh: " + health;
  }

  // =========================================================
  // Canvas sizing
  // =========================================================
  var dpr = window.devicePixelRatio || 1;
  var cw = 0, ch = 0;

  function resizeCanvas() {
    var rect = canvas.getBoundingClientRect();
    cw = rect.width;
    ch = rect.height;
    canvas.width = cw * dpr;
    canvas.height = ch * dpr;
    ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
  }

  window.addEventListener("resize", resizeCanvas);
  resizeCanvas();

  // =========================================================
  // Layout: deterministic ring-based
  // =========================================================
  function computeLayout() {
    nodePositions = {};
    if (!state.nodes.length) return;

    var cx = cw / 2;
    var cy = ch / 2;
    var maxR = Math.min(cw, ch) * 0.38;

    // Group by layer
    var layers = {};
    for (var i = 0; i < state.nodes.length; i++) {
      var n = state.nodes[i];
      var l = n.layer || 0;
      if (!layers[l]) layers[l] = [];
      layers[l].push(n);
    }

    // Sort each layer by MAC for stability
    for (var lk in layers) {
      layers[lk].sort(function (a, b) { return a.mac < b.mac ? -1 : 1; });
    }

    // Place layer 0 at center
    if (layers[0]) {
      for (var j = 0; j < layers[0].length; j++) {
        var angle = layers[0].length === 1 ? 0 : (2 * Math.PI * j) / layers[0].length - Math.PI / 2;
        nodePositions[layers[0][j].mac] = {
          x: cx + (layers[0].length === 1 ? 0 : maxR * 0.15) * Math.cos(angle),
          y: cy + (layers[0].length === 1 ? 0 : maxR * 0.15) * Math.sin(angle)
        };
      }
    }

    // Place subsequent layers on rings
    var layerKeys = Object.keys(layers).sort(function (a, b) { return a - b; });
    for (var li = 1; li < layerKeys.length; li++) {
      var lNodes = layers[layerKeys[li]];
      var radius = maxR * (li / (layerKeys.length - 1 || 1));
      if (layerKeys.length === 2) radius = maxR * 0.65;
      for (var k = 0; k < lNodes.length; k++) {
        var a = (2 * Math.PI * k) / lNodes.length - Math.PI / 2;
        nodePositions[lNodes[k].mac] = {
          x: cx + radius * Math.cos(a),
          y: cy + radius * Math.sin(a)
        };
      }
    }
  }

  // =========================================================
  // Drawing helpers
  // =========================================================
  function nodeColor(role) {
    if (role === "TX") return COL_TX;
    if (role === "COMBO") return COL_COMBO;
    return COL_RX;
  }

  function edgeColor(rssi) {
    if (rssi > -60) return COL_EDGE_GOOD;
    if (rssi >= -75) return COL_EDGE_OK;
    return COL_EDGE_BAD;
  }

  function shortMac(mac) {
    return mac ? mac.slice(-5) : "";
  }

  function formatUptime(ms) {
    var s = Math.floor(ms / 1000);
    var h = Math.floor(s / 3600);
    var m = Math.floor((s % 3600) / 60);
    s = s % 60;
    return h + "h " + m + "m " + s + "s";
  }

  // =========================================================
  // Draw edges
  // =========================================================
  function drawEdges() {
    for (var i = 0; i < state.nodes.length; i++) {
      var n = state.nodes[i];
      if (!n.parent) continue;
      var from = nodePositions[n.parent];
      var to = nodePositions[n.mac];
      if (!from || !to) continue;

      ctx.beginPath();
      ctx.moveTo(from.x, from.y);
      ctx.lineTo(to.x, to.y);
      var col = edgeColor(n.rssi);
      ctx.strokeStyle = col;
      ctx.lineWidth = n.rssi > -60 ? 2.5 : 2;
      if (n.stale) {
        ctx.setLineDash([4, 4]);
        ctx.globalAlpha = 0.4;
      }
      ctx.stroke();
      ctx.setLineDash([]);
      ctx.globalAlpha = 1;
    }
  }

  // =========================================================
  // Data flow particles
  // =========================================================
  function drawFlowParticles(t) {
    // Find TX nodes that are streaming
    var txNodes = [];
    for (var i = 0; i < state.nodes.length; i++) {
      if (state.nodes[i].streaming && state.nodes[i].role === "TX") {
        txNodes.push(state.nodes[i]);
      }
    }
    if (!txNodes.length) return;

    // For each edge where parent is streaming TX or is downstream of TX
    for (var j = 0; j < state.nodes.length; j++) {
      var n = state.nodes[j];
      if (!n.parent || !n.streaming) continue;
      var from = nodePositions[n.parent];
      var to = nodePositions[n.mac];
      if (!from || !to) continue;

      // 3 pulses per edge
      for (var p = 0; p < 3; p++) {
        var frac = ((t * 0.4 + p / 3) % 1);
        var px = from.x + (to.x - from.x) * frac;
        var py = from.y + (to.y - from.y) * frac;

        ctx.beginPath();
        ctx.arc(px, py, 3, 0, Math.PI * 2);
        ctx.fillStyle = COL_CYAN;
        ctx.globalAlpha = 0.8 - frac * 0.4;
        ctx.fill();
        ctx.globalAlpha = 1;
      }
    }
  }

  // =========================================================
  // Draw nodes
  // =========================================================
  var NODE_R = 18;
  var ROOT_R = 24;

  function drawNodes(t) {
    for (var i = 0; i < state.nodes.length; i++) {
      var n = state.nodes[i];
      var pos = nodePositions[n.mac];
      if (!pos) continue;

      var r = n.root ? ROOT_R : NODE_R;
      var col = nodeColor(n.role);

      // Stale: reduced opacity
      if (n.stale) ctx.globalAlpha = 0.5;

      // "You are here" pulsing ring
      if (n.mac === state.self) {
        var pulse = 0.3 + 0.3 * Math.sin(t * 3);
        ctx.beginPath();
        ctx.arc(pos.x, pos.y, r + 8 + 3 * Math.sin(t * 2), 0, Math.PI * 2);
        ctx.strokeStyle = COL_CYAN;
        ctx.lineWidth = 2;
        ctx.globalAlpha = pulse * (n.stale ? 0.5 : 1);
        ctx.stroke();
        ctx.globalAlpha = n.stale ? 0.5 : 1;
      }

      // Node circle
      ctx.beginPath();
      ctx.arc(pos.x, pos.y, r, 0, Math.PI * 2);
      ctx.fillStyle = col;
      ctx.fill();

      // Stale dashed border
      if (n.stale) {
        ctx.setLineDash([3, 3]);
        ctx.strokeStyle = "#888";
        ctx.lineWidth = 1.5;
        ctx.stroke();
        ctx.setLineDash([]);
      }

      // Root crown: simple star above node
      if (n.root) {
        drawStar(pos.x, pos.y - r - 10, 6, 5);
      }

      // "YOU" label for self node
      if (n.mac === state.self) {
        ctx.fillStyle = COL_CYAN;
        ctx.font = "bold 10px sans-serif";
        ctx.textAlign = "center";
        ctx.fillText("YOU", pos.x, pos.y - r - (n.root ? 18 : 14));
      }

      // Streaming indicator dot
      if (n.streaming) {
        ctx.beginPath();
        ctx.arc(pos.x + r + 5, pos.y - r + 5, 4, 0, Math.PI * 2);
        ctx.fillStyle = "#00d9ff";
        ctx.fill();
      }

      // Role letter inside node
      ctx.fillStyle = "#fff";
      ctx.font = "bold " + (n.root ? 13 : 11) + "px sans-serif";
      ctx.textAlign = "center";
      ctx.textBaseline = "middle";
      ctx.fillText(n.role, pos.x, pos.y);

      // MAC label below
      ctx.fillStyle = "#b0b0b0";
      ctx.font = "10px sans-serif";
      ctx.textBaseline = "top";
      ctx.fillText(shortMac(n.mac), pos.x, pos.y + r + 4);

      ctx.globalAlpha = 1;
    }
  }

  function drawStar(cx, cy, outerR, points) {
    var innerR = outerR * 0.45;
    ctx.beginPath();
    for (var i = 0; i < points * 2; i++) {
      var a = (Math.PI * i) / points - Math.PI / 2;
      var rad = i % 2 === 0 ? outerR : innerR;
      if (i === 0) ctx.moveTo(cx + rad * Math.cos(a), cy + rad * Math.sin(a));
      else ctx.lineTo(cx + rad * Math.cos(a), cy + rad * Math.sin(a));
    }
    ctx.closePath();
    ctx.fillStyle = "#ffd700";
    ctx.fill();
  }

  // =========================================================
  // Selection / detail panel
  // =========================================================
  function findNodeAt(x, y) {
    for (var i = state.nodes.length - 1; i >= 0; i--) {
      var n = state.nodes[i];
      var pos = nodePositions[n.mac];
      if (!pos) continue;
      var r = n.root ? ROOT_R : NODE_R;
      var dx = x - pos.x, dy = y - pos.y;
      if (dx * dx + dy * dy <= (r + 6) * (r + 6)) return n;
    }
    return null;
  }

  function onCanvasClick(e) {
    var rect = canvas.getBoundingClientRect();
    var x = e.clientX - rect.left;
    var y = e.clientY - rect.top;
    // Touch support
    if (e.touches && e.touches.length) {
      x = e.touches[0].clientX - rect.left;
      y = e.touches[0].clientY - rect.top;
    }
    var node = findNodeAt(x, y);
    if (node) {
      selectedMac = node.mac;
      updateDetailPanel(node.mac);
      showPanel();
    } else {
      hidePanel();
    }
  }

  canvas.addEventListener("click", onCanvasClick);
  canvas.addEventListener("touchstart", function (e) {
    e.preventDefault();
    onCanvasClick(e);
  }, { passive: false });

  panelClose.addEventListener("click", hidePanel);

  function showPanel() {
    detailPanel.className = "panel-visible";
  }

  function hidePanel() {
    detailPanel.className = "panel-hidden";
    selectedMac = null;
  }

  function updateDetailPanel(mac) {
    var node = null;
    for (var i = 0; i < state.nodes.length; i++) {
      if (state.nodes[i].mac === mac) { node = state.nodes[i]; break; }
    }
    if (!node) { hidePanel(); return; }

    var rssiClass = node.rssi > -60 ? "rssi-good" : (node.rssi >= -75 ? "rssi-ok" : "rssi-bad");
    var roleBadge = "badge-" + node.role.toLowerCase();
    if (node.role === "COMBO") roleBadge = "badge-combo";

    var rssiBar = "";
    if (node.rssi !== 0) {
      var bars = node.rssi > -55 ? "▂▄▆█" : (node.rssi > -65 ? "▂▄▆" : (node.rssi > -75 ? "▂▄" : "▂"));
      rssiBar = '<span class="' + rssiClass + '">' + bars + " " + node.rssi + " dBm</span>";
    } else {
      rssiBar = "N/A (root)";
    }

    var isSelf = node.mac === state.self;

    panelBody.innerHTML =
      '<div class="detail-row"><span class="detail-label">MAC</span><span class="detail-value">' + node.mac + (isSelf ? ' <small>(you)</small>' : '') + '</span></div>' +
      '<div class="detail-row"><span class="detail-label">Role</span><span class="detail-value"><span class="badge ' + roleBadge + '">' + node.role + '</span></span></div>' +
      '<div class="detail-row"><span class="detail-label">Layer</span><span class="detail-value">' + node.layer + '</span></div>' +
      '<div class="detail-row"><span class="detail-label">RSSI</span><span class="detail-value">' + rssiBar + '</span></div>' +
      '<div class="detail-row"><span class="detail-label">Children</span><span class="detail-value">' + node.children + '</span></div>' +
      '<div class="detail-row"><span class="detail-label">Streaming</span><span class="detail-value">' + (node.streaming ? "✅ Yes" : "❌ No") + '</span></div>' +
      '<div class="detail-row"><span class="detail-label">Uptime</span><span class="detail-value">' + formatUptime(node.uptime) + '</span></div>' +
      '<div class="detail-row"><span class="detail-label">Parent</span><span class="detail-value">' + (node.parent || "— (root)") + '</span></div>' +
      (node.stale ? '<div class="detail-row" style="color:#ff4444"><span class="detail-label">Status</span><span class="detail-value">⚠ Stale</span></div>' : '');
  }

  // =========================================================
  // Animation loop (~30fps)
  // =========================================================
  var frameDuration = 1000 / 30;

  function loop(now) {
    requestAnimationFrame(loop);
    if (now - lastFrame < frameDuration) return;
    lastFrame = now;
    animT = now / 1000;

    resizeCanvas();
    computeLayout();

    // Clear
    ctx.clearRect(0, 0, cw, ch);

    // Draw
    drawEdges();
    drawFlowParticles(animT);
    drawNodes(animT);
  }

  requestAnimationFrame(loop);

  // =========================================================
  // Init
  // =========================================================
  wsConnect();
})();
