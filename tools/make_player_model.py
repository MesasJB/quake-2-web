#!/usr/bin/env python3
"""
Crea un modelo de jugador (players/male/grunt) para Quake 2 web a partir del soldado
que YA viene dentro de tu pak0.pak, porque ese pak no trae los modelos de jugador
(sin ellos los otros jugadores se ven como un rombo).

Uso:  python make_player_model.py ruta/a/pak0.pak  Qwasm2-main/wasm/baseq2
Escribe:  <salida>/players/male/tris.md2, grunt.pcx, grunt_i.pcx
"""
import os, struct, sys

# ---- orden de frames de un modelo de jugador de Quake 2 (199 frames) -> nombres de frames del soldado ----
def seq(prefix, n, start=1):
    return ['%s%02d' % (prefix, start + i) if prefix in ('run', 'duck') else '%s%02d' % (prefix, start + i) for i in range(n)]

def S(p, a, b, step=1):                      # soldier frames p+a .. p+b
    return [p + str(i) for i in range(a, b + 1, step)]

def spread(prefix, first, last, n):          # n muestras repartidas en toda la secuencia (la ultima = pose final)
    idx = [first + round(i * (last - first) / (n - 1)) for i in range(n)]
    return [prefix + str(i) for i in idx]

def cyc(lst, n):
    return [lst[i % len(lst)] for i in range(n)]

def player_frames():
    f = []
    f += cyc(S('stand', 101, 130), 40)                              # 0-39   parado
    f += ['run%02d' % i for i in (1, 3, 5, 7, 9, 11)]               # 40-45  correr
    f += S('attak', 101, 108)                                       # 46-53  disparar
    f += S('pain', 101, 104) + S('pain', 201, 204) + S('pain', 301, 304)   # 54-65 dolor
    f += ['run05'] * 6                                              # 66-71  salto
    f += ['run%02d' % i for i in range(1, 13)]                      # 72-83  voltereta
    f += S('stand', 301, 311)                                       # 84-94  saludo
    f += S('stand', 312, 328)                                       # 95-111 burla
    f += S('stand', 329, 339)                                       # 112-122 saludar con la mano
    f += S('attak', 101, 112)                                       # 123-134 señalar
    f += ['duck03'] * 19                                            # 135-153 agachado
    f += ['duck01', 'duck02', 'duck03', 'duck04', 'duck05', 'duck03']   # 154-159 caminar agachado
    f += S('attak', 301, 309)                                       # 160-168 disparar agachado
    f += S('pain', 401, 404)                                        # 169-172 dolor agachado
    f += spread('death', 101, 136, 5)                               # 173-177 muerte agachado
    f += spread('death', 101, 136, 6)                               # 178-183 muerte 1
    f += spread('death', 201, 235, 6)                               # 184-189 muerte 2
    f += spread('death', 301, 345, 8)                               # 190-197 muerte 3
    f += [f[-1]]                                                    # 198
    assert len(f) == 199, len(f)
    return f

# ---- PAK ----
def read_pak(path):
    f = open(path, 'rb'); f.seek(4); off, ln = struct.unpack('<ii', f.read(8)); f.seek(off)
    ents = {}
    for _ in range(ln // 64):
        d = f.read(64); ents[d[:56].split(b'\0')[0].decode('latin1')] = struct.unpack('<ii', d[56:64])
    def get(n):
        o, l = ents[n]; f.seek(o); return f.read(l)
    return get

# ---- MD2 ----
def build_md2(src):
    names = 'ident ver skinw skinh framesize nskins nxyz nst ntris nglcmds nframes ofs_skins ofs_st ofs_tris ofs_frames ofs_glcmds ofs_end'.split()
    H = dict(zip(names, struct.unpack('<17i', src[:68])))
    assert H['ident'] == 844121161 and H['ver'] == 8
    fs = H['framesize']
    frames = {}
    for i in range(H['nframes']):
        b = H['ofs_frames'] + i * fs
        frames[src[b + 24:b + 40].split(b'\0')[0].decode()] = src[b:b + fs]
    want = player_frames()
    body = b''.join(frames[n] for n in want)
    head = bytearray(src[:H['ofs_frames']])
    ofs_glcmds = H['ofs_frames'] + len(want) * fs
    glcmds = src[H['ofs_glcmds']:H['ofs_glcmds'] + H['nglcmds'] * 4]
    ofs_end = ofs_glcmds + len(glcmds)
    struct.pack_into('<i', head, 10 * 4, len(want))      # num_frames
    struct.pack_into('<i', head, 15 * 4, ofs_glcmds)     # ofs_glcmds
    struct.pack_into('<i', head, 16 * 4, ofs_end)        # ofs_end
    return bytes(head) + body + glcmds

# ---- PCX (icono 32x32 con la misma paleta) ----
def pcx_decode(d):
    xmin, ymin, xmax, ymax = struct.unpack('<4H', d[4:12]); w, h = xmax - xmin + 1, ymax - ymin + 1
    bpl = struct.unpack('<H', d[66:68])[0]; pal = d[-768:]
    i, rows = 128, []
    for _ in range(h):
        row = bytearray()
        while len(row) < bpl:
            b = d[i]; i += 1
            if (b & 0xC0) == 0xC0: row += bytes([d[i]]) * (b & 0x3F); i += 1
            else: row.append(b)
        rows.append(bytes(row[:w]))
    return w, h, rows, pal

def pcx_encode(w, h, rows, pal):
    bpl = w + (w & 1)
    hdr = bytearray(128); hdr[0] = 10; hdr[1] = 5; hdr[2] = 1; hdr[3] = 8
    struct.pack_into('<4H', hdr, 4, 0, 0, w - 1, h - 1); struct.pack_into('<2H', hdr, 12, 72, 72)
    hdr[65] = 1; struct.pack_into('<H', hdr, 66, bpl); struct.pack_into('<H', hdr, 68, 1)
    out = bytearray(hdr)
    for r in rows:
        r = r + bytes(bpl - w); i = 0
        while i < bpl:
            v = r[i]; n = 1
            while i + n < bpl and r[i + n] == v and n < 63: n += 1
            if n > 1 or v >= 0xC0: out += bytes([0xC0 | n, v])
            else: out.append(v)
            i += n
    return bytes(out) + b'\x0c' + pal

def icon(pcx):
    w, h, rows, pal = pcx_decode(pcx)
    small = [bytes(rows[int(y * h / 32)][int(x * w / 32)] for x in range(32)) for y in range(32)]
    return pcx_encode(32, 32, small, pal)

if __name__ == '__main__':
    pak, out = sys.argv[1], sys.argv[2]
    get = read_pak(pak)
    d = os.path.join(out, 'players', 'male'); os.makedirs(d, exist_ok=True)
    open(os.path.join(d, 'tris.md2'), 'wb').write(build_md2(get('models/monsters/soldier/tris.md2')))
    skin = get('models/monsters/soldier/skin.pcx')
    open(os.path.join(d, 'grunt.pcx'), 'wb').write(skin)
    open(os.path.join(d, 'grunt_i.pcx'), 'wb').write(icon(skin))
    print('Listo:', d)
