// Quake 2 web: proxy WebSocket <-> UDP + administrador de salas (sin dependencias, Node 16+).
// - Cada navegador se conecta por WebSocket (?room=ID) y recibe su propio socket UDP hacia la sala elegida.
// - Cada sala es un servidor dedicado de Quake 2 (q2ded) en su propio puerto UDP.
// - POST /rooms crea una sala nueva, GET /rooms las lista. Las salas vacias se cierran solas.
const http = require('http');
const crypto = require('crypto');
const dgram = require('dgram');
const { spawn } = require('child_process');

const HTTP_PORT   = parseInt(process.env.PORT || '8080', 10);
const Q2_HOST     = process.env.Q2_HOST || '127.0.0.1';
const Q2_DIR      = process.env.Q2_DIR || '/q2';
const Q2_BIN      = process.env.Q2_BIN || './q2ded';
const MAIN_PORT   = parseInt(process.env.Q2_PORT || '27910', 10);   // sala principal
const CLIENT_PORT = 27910;                                          // puerto al que "cree" conectarse el navegador
const MAX_EXTRA   = parseInt(process.env.MAX_ROOMS || '3', 10);     // salas creadas por jugadores
const MAX_WS      = parseInt(process.env.MAX_WS || '64', 10);
const IDLE_MS     = parseInt(process.env.ROOM_IDLE_MS || String(10 * 60 * 1000), 10);
const MAX_AGE_MS  = 6 * 60 * 60 * 1000;
const START_MAIN  = process.env.START_MAIN !== '0';
const GUID = '258EAFA5-E914-47DA-95CA-C5AB0DC85B11';

const SP_MAPS = ['base1','base2','base3','train','bunk1','ware1','ware2','jail1','jail2','jail3','jail4','jail5','security',
  'mintro','mine1','mine2','mine3','mine4','fact1','fact2','fact3','power1','power2','cool1','waste1','waste2','waste3',
  'biggun','hangar1','hangar2','lab','command','strike','space','city1','city2','city3','boss1','boss2'];
const DM_MAPS = ['q2dm1','q2dm2','q2dm3','q2dm4','q2dm5','q2dm6','q2dm7','q2dm8'];
const ALLOWED_MAPS = SP_MAPS.concat(process.env.Q2_DM === '1' ? DM_MAPS : []);

let clients = 0;
const rooms = new Map();           // id -> sala
const usedPorts = new Set([MAIN_PORT]);
const lastCreate = new Map();      // ip -> timestamp

// ---------------- salas ----------------
function spawnRoom(room) {
  const args = ['+set', 'dedicated', '1', '+set', 'port', String(room.port), '+set', 'maxclients', String(room.maxclients)];
  if (room.mode === 'deathmatch') args.push('+set', 'deathmatch', '1', '+set', 'coop', '0');
  else args.push('+set', 'coop', '1', '+set', 'deathmatch', '0');
  args.push('+map', room.map);
  const p = spawn(Q2_BIN, args, { cwd: Q2_DIR, stdio: 'ignore' });
  p.on('error', (e) => { console.error('No se pudo iniciar la sala', room.id, e.message); rooms.delete(room.id); usedPorts.delete(room.port); });
  p.on('exit', () => { rooms.delete(room.id); usedPorts.delete(room.port); console.log('Sala cerrada:', room.id); });
  room.proc = p;
}
function newRoom(opts) {
  const room = Object.assign({ created: Date.now(), emptySince: Date.now(), players: 0 }, opts);
  rooms.set(room.id, room); usedPorts.add(room.port); spawnRoom(room);
  console.log('Sala creada:', room.id, room.name, room.map, room.mode, 'puerto', room.port);
  return room;
}
function freePort() { for (let p = MAIN_PORT + 1; p < MAIN_PORT + 60; p++) if (!usedPorts.has(p)) return p; return 0; }
const cleanName = (s) => String(s || '').replace(/[^A-Za-z0-9 _\-]/g, '').trim().slice(0, 20);

function queryStatus(port, cb) {
  const u = dgram.createSocket('udp4');
  let done = false;
  const finish = (v) => { if (done) return; done = true; try { u.close(); } catch (e) {} cb(v); };
  const t = setTimeout(() => finish(null), 1200);
  u.on('message', (m) => {
    clearTimeout(t);
    const lines = m.toString('latin1').split('\n');
    const info = {};
    const kv = (lines[1] || '').split('\\').slice(1);
    for (let i = 0; i + 1 < kv.length; i += 2) info[kv[i]] = kv[i + 1];
    const names = lines.slice(2).filter(l => l.trim()).map(l => { const q = l.match(/"(.*)"/); return q ? q[1] : l.trim(); });
    finish({ map: info.mapname || '?', maxclients: parseInt(info.maxclients || '0', 10),
             mode: info.coop === '1' ? 'coop' : (info.deathmatch === '1' ? 'deathmatch' : 'campaña'), players: names.length, names });
  });
  u.on('error', () => finish(null));
  u.send(Buffer.concat([Buffer.from([255, 255, 255, 255]), Buffer.from('status\n')]), port, Q2_HOST);
}
function listRooms(cb) {
  const all = Array.from(rooms.values());
  if (!all.length) return cb([]);
  let left = all.length; const out = [];
  all.forEach((r) => queryStatus(r.port, (st) => {
    if (st) { r.players = st.players; if (st.players > 0) r.emptySince = 0; else if (!r.emptySince) r.emptySince = Date.now(); }
    out.push({ id: r.id, name: r.name, map: r.map, mode: r.mode, players: st ? st.players : 0, maxclients: r.maxclients,
               main: !!r.main, online: !!st, names: st ? st.names : [] });
    if (--left === 0) cb(out.sort((a, b) => (b.main - a.main) || a.name.localeCompare(b.name)));
  }));
}
setInterval(() => {   // cierra salas vacias o muy viejas
  const now = Date.now();
  rooms.forEach((r) => {
    if (r.main) return;
    queryStatus(r.port, (st) => {
      if (st && st.players > 0) { r.emptySince = 0; return; }
      if (!r.emptySince) r.emptySince = now;
      if (now - r.emptySince > IDLE_MS || now - r.created > MAX_AGE_MS) { try { r.proc.kill(); } catch (e) {} }
    });
  });
}, 30000).unref();
function killAll() { rooms.forEach((r) => { try { r.proc.kill(); } catch (e) {} }); }
process.on('exit', killAll); process.on('SIGTERM', () => process.exit(0)); process.on('SIGINT', () => process.exit(0));

if (START_MAIN) {
  const mode = process.env.Q2_MODE === 'dm' ? 'deathmatch' : 'coop';
  newRoom({ id: 'main', name: 'Sala principal', map: process.env.Q2_MAP || (mode === 'coop' ? 'base1' : 'q2dm1'),
            mode, port: MAIN_PORT, maxclients: mode === 'coop' ? 4 : 8, main: true });
}

// ---------------- HTTP ----------------
const CORS = { 'Access-Control-Allow-Origin': '*', 'Access-Control-Allow-Methods': 'GET,POST,OPTIONS', 'Access-Control-Allow-Headers': 'Content-Type' };
function json(res, code, obj) { res.writeHead(code, Object.assign({ 'Content-Type': 'application/json' }, CORS)); res.end(JSON.stringify(obj)); }

const server = http.createServer((req, res) => {
  const path = req.url.split('?')[0];
  if (req.method === 'OPTIONS') { res.writeHead(204, CORS); return res.end(); }

  if (req.method === 'GET' && path === '/rooms') return listRooms((l) => json(res, 200, { rooms: l, maxExtra: MAX_EXTRA, maps: ALLOWED_MAPS }));

  if (req.method === 'GET' && path === '/status') {           // compatibilidad: estado de la sala principal
    return listRooms((l) => { const m = l.find(r => r.main) || l[0] || null;
      json(res, 200, { online: !!(m && m.online), ws_clients: clients, room: m && { hostname: m.name, map: m.map, mode: m.mode, players: m.players, maxclients: m.maxclients } }); });
  }

  if (req.method === 'POST' && path === '/rooms') {
    let body = '';
    req.on('data', (c) => { body += c; if (body.length > 2048) req.destroy(); });
    req.on('end', () => {
      let d; try { d = JSON.parse(body || '{}'); } catch (e) { return json(res, 400, { error: 'JSON invalido' }); }
      const ip = req.socket.remoteAddress, now = Date.now();
      if (now - (lastCreate.get(ip) || 0) < 20000) return json(res, 429, { error: 'Esperá unos segundos antes de crear otra sala.' });
      const extra = Array.from(rooms.values()).filter(r => !r.main).length;
      if (extra >= MAX_EXTRA) return json(res, 409, { error: 'Ya hay ' + MAX_EXTRA + ' salas creadas. Esperá a que se cierre alguna o unite a una.' });
      const name = cleanName(d.name); if (!name) return json(res, 400, { error: 'Poné un nombre para la sala.' });
      if (Array.from(rooms.values()).some(r => r.name.toLowerCase() === name.toLowerCase())) return json(res, 409, { error: 'Ya existe una sala con ese nombre.' });
      const map = String(d.map || 'base1'); if (ALLOWED_MAPS.indexOf(map) < 0) return json(res, 400, { error: 'Mapa no disponible.' });
      const mode = d.mode === 'deathmatch' ? 'deathmatch' : 'coop';
      let maxc = parseInt(d.maxclients || '4', 10); if (!(maxc >= 2)) maxc = 4; maxc = Math.min(maxc, mode === 'coop' ? 4 : 8);
      const port = freePort(); if (!port) return json(res, 503, { error: 'No hay puertos libres.' });
      lastCreate.set(ip, now);
      const id = crypto.randomBytes(4).toString('hex');
      const r = newRoom({ id, name, map, mode, port, maxclients: maxc });
      json(res, 201, { id: r.id, name: r.name, map: r.map, mode: r.mode, maxclients: r.maxclients });
    });
    return;
  }

  res.writeHead(200, Object.assign({ 'Content-Type': 'text/plain' }, CORS));
  res.end('Quake 2 proxy OK. Jugadores conectados: ' + clients + ' - salas: ' + rooms.size + '\n');
});

// ---------------- WebSocket <-> UDP ----------------
function frame(opcode, payload) {
  const len = payload.length; let head;
  if (len < 126) head = Buffer.from([0x80 | opcode, len]);
  else if (len < 65536) { head = Buffer.alloc(4); head[0] = 0x80 | opcode; head[1] = 126; head.writeUInt16BE(len, 2); }
  else { head = Buffer.alloc(10); head[0] = 0x80 | opcode; head[1] = 127; head.writeBigUInt64BE(BigInt(len), 2); }
  return Buffer.concat([head, payload]);
}

server.on('upgrade', (req, socket) => {
  const key = req.headers['sec-websocket-key'];
  const q = new URL(req.url, 'http://x').searchParams;
  const room = rooms.get(q.get('room') || 'main');
  if (!key || clients >= MAX_WS || !room) { socket.end('HTTP/1.1 ' + (room ? '503 Service Unavailable' : '404 Not Found') + '\r\n\r\n'); return; }
  const accept = crypto.createHash('sha1').update(key + GUID).digest('base64');
  const protos = (req.headers['sec-websocket-protocol'] || '').split(',').map(s => s.trim()).filter(Boolean);
  let h = 'HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: ' + accept + '\r\n';
  if (protos.length) h += 'Sec-WebSocket-Protocol: ' + protos[0] + '\r\n';
  socket.write(h + '\r\n');
  socket.setNoDelay(true);
  clients++;

  const udp = dgram.createSocket('udp4');
  let closed = false, buf = Buffer.alloc(0), partial = null;
  const close = () => { if (closed) return; closed = true; clients--; try { udp.close(); } catch (e) {} socket.destroy(); };

  // Siempre anunciamos CLIENT_PORT: es el puerto que el motor del navegador cree estar usando
  const portMsg = Buffer.from([0xff, 0xff, 0xff, 0xff, 0x70, 0x6f, 0x72, 0x74, (CLIENT_PORT >> 8) & 255, CLIENT_PORT & 255]);
  socket.write(frame(0x2, portMsg));

  udp.on('message', msg => { if (!closed) socket.write(frame(0x2, msg)); });
  udp.on('error', close);

  socket.on('data', chunk => {
    buf = Buffer.concat([buf, chunk]);
    while (buf.length >= 2) {
      const fin = (buf[0] & 0x80) !== 0, op = buf[0] & 0x0f, masked = (buf[1] & 0x80) !== 0;
      let len = buf[1] & 0x7f, off = 2;
      if (len === 126) { if (buf.length < 4) return; len = buf.readUInt16BE(2); off = 4; }
      else if (len === 127) { if (buf.length < 10) return; len = Number(buf.readBigUInt64BE(2)); off = 10; }
      if (len > 65536) return close();
      if (buf.length < off + (masked ? 4 : 0) + len) return;
      let payload;
      if (masked) {
        const mask = buf.subarray(off, off + 4); off += 4;
        payload = Buffer.from(buf.subarray(off, off + len));
        for (let i = 0; i < len; i++) payload[i] ^= mask[i & 3];
      } else payload = Buffer.from(buf.subarray(off, off + len));
      buf = buf.subarray(off + len);

      if (op === 0x8) { try { socket.write(frame(0x8, Buffer.alloc(0))); } catch (e) {} return close(); }
      if (op === 0x9) { socket.write(frame(0xA, payload)); continue; }
      if (op === 0xA) continue;
      if (op === 0x1 || op === 0x2 || op === 0x0) {
        partial = op === 0x0 && partial ? Buffer.concat([partial, payload]) : payload;
        if (fin) { udp.send(partial, room.port, Q2_HOST); partial = null; }
      }
    }
  });
  socket.on('end', close); socket.on('close', close); socket.on('error', close);
});

server.listen(HTTP_PORT, () => console.log('Proxy escuchando en :' + HTTP_PORT));
