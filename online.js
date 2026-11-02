/*
 * online.js -- capa online del lado del navegador (Quake 2 web).
 * Misma estructura que el Quake 1 web:
 *  - Mantiene el WebSocket con el servidor relay (server/server.js).
 *  - Le pasa paquetes al motor (src/backends/wasm/network.c) y recibe sus comandos.
 *  - Dibuja la lista de salas al costado y la ventanita "name?:".
 *
 * Servidor por defecto: window.Q2_DEFAULT_SERVER (se define en index.html antes de cargar este archivo).
 */
(function () {
  'use strict';

  var Q = window.QOnline = {};
  var NAME_KEY = 'q2_nick', SRV_KEY = 'q2server';

  /* ---------- utilidades ---------- */
  function $(id) { return document.getElementById(id); }
  function lsGet(k) { try { return window.localStorage.getItem(k); } catch (e) { return null; } }
  function lsSet(k, v) { try { window.localStorage.setItem(k, v); } catch (e) { /* ignorar */ } }

  // Nombres de Quake: ASCII imprimible, sin comillas / ; \ ni //, maximo 15
  function cleanName(s) {
    return String(s || '').replace(/[^\x20-\x7e]/g, '').replace(/["\\;\/$]/g, '').replace(/\s+/g, ' ').trim().slice(0, 15);
  }

  /* ---------- estado ---------- */
  var ws = null, myId = 0, epoch = 0, started = false;
  var rx = [], cmds = [], rooms = [];
  var wantListen = false, roomInfo = null;
  var candidates = [], candIdx = 0, openedThisTry = false, retryTimer = null, retryMs = 1500;

  function sameOrigin() {
    if (/^https?:$/.test(location.protocol)) return (location.protocol === 'https:' ? 'wss://' : 'ws://') + location.host;
    return 'ws://localhost:8765';
  }
  function normUrl(u) {
    u = String(u || '').trim(); if (!u) return '';
    u = u.replace(/^http:\/\//i, 'ws://').replace(/^https:\/\//i, 'wss://');
    if (!/^wss?:\/\//i.test(u)) u = 'wss://' + u;
    return u.replace(/\/+$/, '');
  }
  // Pagina abierta desde esta PC / red local: el relay suele estar en el mismo lugar.
  // Pagina publica (ej. GitHub Pages): el relay es el servidor por defecto.
  function isLocalHost() {
    var h = location.hostname;
    return !h || h === 'localhost' || /^(127\.|10\.|192\.168\.|172\.(1[6-9]|2\d|3[01])\.)/.test(h) || /\.local$/.test(h);
  }
  function buildCandidates() {
    var list = [], saved = lsGet(SRV_KEY), def = normUrl(window.Q2_DEFAULT_SERVER);
    if (saved) list.push(saved);
    if (def && !isLocalHost()) list.push(def);
    list.push(sameOrigin());
    list.push((location.protocol === 'https:' ? 'wss://' : 'ws://') + (location.hostname || 'localhost') + ':8765');
    if (def && isLocalHost()) list.push(def);
    candidates = list.filter(function (u, i) { return list.indexOf(u) === i; });
    candIdx = 0;
  }

  /* ---------- conexion ---------- */
  function sendJson(o) { if (ws && ws.readyState === 1) ws.send(JSON.stringify(o)); }

  function connect() {
    clearTimeout(retryTimer);
    var url = candidates[candIdx % candidates.length];
    openedThisTry = false;
    setStatus('connecting', 'Conectando a ' + url + '\u2026');
    try { ws = new WebSocket(url); } catch (e) { return scheduleRetry(); }
    ws.binaryType = 'arraybuffer';
    var mine = ws;
    ws.onopen = function () {
      openedThisTry = true; retryMs = 1500;
      sendJson({ t: 'rooms' });
      if (wantListen) sendJson({ t: 'listen', on: true });
      if (roomInfo) sendJson(roomInfo);
    };
    ws.onmessage = function (ev) {
      if (typeof ev.data === 'string') {
        var m; try { m = JSON.parse(ev.data); } catch (e) { return; }
        if (m.t === 'hello') { myId = m.id | 0; setStatus('ok', 'Conectado \u00b7 tu direcci\u00f3n: #' + myId); renderRooms(); }
        else if (m.t === 'rooms') { rooms = Array.isArray(m.rooms) ? m.rooms : []; renderRooms(); }
        return;
      }
      rx.push(new Uint8Array(ev.data));
      if (rx.length > 512) rx.shift();
    };
    ws.onclose = function () {
      if (ws !== mine) return;
      ws = null; myId = 0; epoch++; rx.length = 0; rooms = []; renderRooms();
      if (!openedThisTry) candIdx++;           // si nunca abrio, probamos la siguiente direccion
      scheduleRetry();
    };
    ws.onerror = function () { /* onclose se encarga */ };
  }
  function scheduleRetry() {
    setStatus('bad', 'Sin conexi\u00f3n con el servidor. Reintentando\u2026');
    retryTimer = setTimeout(connect, retryMs);
    retryMs = Math.min(retryMs * 1.5, 6000);
  }

  /* ---------- API que usa el motor (net_web.c) ---------- */
  Q.start = function () {
    if (started) return;
    started = true;
    buildCandidates();
    var saved = cleanName(lsGet(NAME_KEY));
    if (saved) cmds.push('name "' + saved + '"');
    refreshName();
    connect();
  };
  Q.shift = function () { return rx.length ? rx.shift() : null; };
  Q.send = function (u8, unreliable) {
    if (!ws || ws.readyState !== 1) return 0;
    if (unreliable && ws.bufferedAmount > 131072) return 0;   // mejor perder un paquete que acumular lag
    ws.send(u8);
    return 1;
  };
  Q.myId = function () { return myId; };
  Q.epoch = function () { return epoch; };
  Q.setListen = function (on) {
    wantListen = !!on;
    if (!on) roomInfo = null;
    sendJson({ t: 'listen', on: wantListen });
  };
  Q.setRoom = function (name, map, players, max) {
    roomInfo = { t: 'room', name: name, map: map, players: players, max: max };
    sendJson(roomInfo);
  };
  Q.pollCmd = function () { return cmds.length ? cmds.shift() : null; };
  Q.roomCount = function () { return rooms.length; };
  Q.roomLine = function (i) {
    var r = rooms[i]; if (!r) return '';
    function c(s) { return String(s).replace(/\|/g, ' '); }
    return [r.id | 0, c(r.name), c(r.map), r.players | 0, r.max | 0].join('|');
  };
  Q.refreshRooms = function () { sendJson({ t: 'rooms' }); };

  /* ---------- interfaz: estado y lista de salas ---------- */
  function setStatus(kind, text) {
    var dot = $('netDot'), t = $('netText');
    if (dot) dot.className = 'dot ' + kind;
    if (t) t.textContent = text;
  }
  function refreshName() {
    var el = $('myName');
    if (el) el.textContent = cleanName(lsGet(NAME_KEY)) || 'player';
  }

  function renderRooms() {
    var list = $('roomList'); if (!list) return;
    while (list.firstChild) list.removeChild(list.firstChild);
    var cnt = $('roomCount'); if (cnt) cnt.textContent = rooms.length ? String(rooms.length) : '';
    if (!rooms.length) {
      var e = document.createElement('div');
      e.className = 'empty';
      e.textContent = myId ? 'No hay salas todav\u00eda. Cre\u00e1 una desde Multiplayer \u2192 start network server.' : 'Esperando al servidor\u2026';
      list.appendChild(e);
      return;
    }
    rooms.forEach(function (r) {
      var row = document.createElement('div'); row.className = 'room';
      var info = document.createElement('div'); info.className = 'info';
      var n = document.createElement('div'); n.className = 'rname'; n.textContent = String(r.name);
      var d = document.createElement('div'); d.className = 'rmeta';
      d.textContent = (r.map ? String(r.map) + ' \u00b7 ' : '') + (r.players | 0) + '/' + (r.max | 0) + ' jugadores';
      info.appendChild(n); info.appendChild(d); row.appendChild(info);
      if (r.id === myId) {
        var me = document.createElement('span'); me.className = 'mine'; me.textContent = 'Tu sala'; row.appendChild(me);
      } else {
        var b = document.createElement('button'); b.className = 'sec join'; b.textContent = 'Unirse';
        if ((r.players | 0) >= (r.max | 0)) { b.disabled = true; b.textContent = 'Llena'; }
        b.addEventListener('click', function () { joinRoom(r.id); });
        row.appendChild(b);
      }
      list.appendChild(row);
    });
  }

  function joinRoom(id) {
    id = id | 0; if (id <= 0) return;
    var cv = $('canvas');
    try { if (cv && cv.style.display !== 'none') cv.requestPointerLock(); } catch (e) { /* ignorar */ }
    cmds.push('join ' + id);
    notify('Conectando a la sala #' + id + '\u2026');
  }

  /* ---------- avisos ---------- */
  var toastTimer = null;
  function notify(msg) {
    var t = $('toast'); if (!t) return;
    t.textContent = String(msg); t.hidden = false;
    clearTimeout(toastTimer);
    toastTimer = setTimeout(function () { t.hidden = true; }, 4000);
  }
  Q.notify = notify;
  Q.setServer = function (u) { lsSet(SRV_KEY, normUrl(u)); };   // para ?srv=wss://... en la direccion de la pagina

  /* ---------- ventanita "name?:" ---------- */
  var promptEl, inputEl;
  function closePrompt() {
    if (!promptEl || promptEl.hidden) return;
    promptEl.hidden = true;
    var cv = $('canvas'); if (cv) cv.focus();
  }
  function applyName() {
    var v = cleanName(inputEl.value);
    if (v) {
      lsSet(NAME_KEY, v);
      cmds.push('name "' + v + '"');
      refreshName();
      notify('Tu nombre ahora es: ' + v);
    }
    closePrompt();
  }
  Q.askName = function (current) {
    if (!promptEl) return;
    try { if (document.pointerLockElement) document.exitPointerLock(); } catch (e) { /* ignorar */ }
    // El motor recibio el "Enter" que abrio esto; le mandamos su "soltar" para que no quede trabado.
    try {
      document.dispatchEvent(new KeyboardEvent('keyup', { key: 'Enter', code: 'Enter', keyCode: 13, which: 13, bubbles: true }));
    } catch (e) { /* ignorar */ }
    inputEl.value = cleanName(lsGet(NAME_KEY)) || cleanName(current) || '';
    promptEl.hidden = false;
    setTimeout(function () { inputEl.focus(); inputEl.select(); }, 0);
  };

  function initUi() {
    promptEl = $('namePrompt'); inputEl = $('nameInput');
    if (promptEl && inputEl) {
      // El motor (SDL) escucha el teclado en fase de captura: mientras la ventanita esta
      // abierta cortamos las teclas ANTES de que lleguen a el (escribir en el campo sigue
      // funcionando porque no cancelamos la accion por defecto).
      ['keydown', 'keyup', 'keypress'].forEach(function (type) {
        window.addEventListener(type, function (e) {
          if (promptEl.hidden) return;
          e.stopImmediatePropagation();
          if (type !== 'keydown' || e.repeat) return;
          if (e.key === 'Enter') { e.preventDefault(); applyName(); }
          else if (e.key === 'Escape') { e.preventDefault(); closePrompt(); }
        }, true);
      });
      $('nameOk').addEventListener('click', applyName);
      $('nameCancel').addEventListener('click', closePrompt);
    }
    var save = $('srvSave'), inp = $('srvInput');
    if (save && inp) {
      inp.value = lsGet(SRV_KEY) || '';
      inp.placeholder = normUrl(window.Q2_DEFAULT_SERVER) || sameOrigin();
      ['keydown', 'keyup', 'keypress'].forEach(function (type) {
        inp.addEventListener(type, function (e) { e.stopPropagation(); });
      });
      save.addEventListener('click', function () {
        var v = normUrl(inp.value);
        lsSet(SRV_KEY, v);
        if (!started) return;
        buildCandidates();
        if (ws) { try { ws.close(); } catch (e) { /* ignorar */ } } else { retryMs = 1500; connect(); }
      });
    }
    refreshName();
    renderRooms();
  }
  if (document.readyState === 'loading') document.addEventListener('DOMContentLoaded', initUi);
  else initUi();
})();
