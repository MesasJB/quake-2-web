# Quake 2 web: online con la estructura del Quake 1 web

Quake 1 y Quake 2 ahora comparten el mismo esquema:

| Pieza | Quake 1 web | Quake 2 web (este paquete) |
|---|---|---|
| Driver de red del motor | `net_web.c` (entra en la tabla `net_drivers[]`) | `src/backends/wasm/network.c` (reemplaza a `unix/network.c`; Q2 no tiene tabla de drivers, tiene la API `NET_*`) |
| Capa del navegador | `online.js` | `online.js` (mismo, adaptado) |
| Relay con salas | `server/server.js` | `server/server.js` (mismo protocolo) |
| Gancho por frame | `WebNet_Tick()` en `Host_Frame` | `WebNet_Tick()` dentro de `wasm_poll_commands()` (ya se llamaba por frame) |
| Nombre | ventana `name?:` al entrar a TCP/IP | ventana `name?:` al entrar a join/start network server |
| Unirse | boton "Unirse" -> `connect "#12"` | idem |
| Lista en el menu | "Search for local games" | "search for local servers" (se contesta con las salas del relay) |

**Cambio de modelo:** antes las salas eran servidores dedicados `q2ded` dentro de Docker (proxy WebSocket<->UDP).
Ahora, como en Q1, **el host es un jugador**: crea la partida con *Multiplayer > start network server*, su
navegador corre el servidor y aparece en la lista; los demas entran con "Unirse" o `connect #12`.
El servidor ya no necesita compilar el juego ni tener pak0.

## Como aplicarlo
Copia el contenido de esta carpeta sobre tu repo `quake2-completo` (mismas rutas), y despues:

1. **Makefile** (una vez, desde la raiz del repo): `patch -p1 < makefile-wasm-network.patch`
   (o a mano: en la build Emscripten reemplazar `src/backends/unix/network.o` por `src/backends/wasm/network.o`).
2. Recompilar el wasm como siempre (el unico `.c` nuevo es `network.c`).
3. Subir `index.html` y `online.js` junto a `index.js/.wasm/.data`.
4. Redesplegar el relay (`server/Dockerfile` ya es solo Node). Si tu URL de Render cambia, editala en `index.html`.

## Que se probo (sin emsdk en esta maquina)
- `network.c` compila con `-Wall -Wextra` contra los headers reales de Q2 y paso 38 pruebas con un relay simulado
  (cliente, host, rechazos, busqueda de salas, comando join, cortes).
- `server.js` y `online.js` se probaron con clientes WebSocket reales (salas, handshake, datagramas de 1400 bytes,
  anti-flood, reconexion).
- **No se pudo probar** la compilacion con Emscripten ni jugar en un navegador: esa prueba final es tuya.

## Si algo falla al compilar/probar
- `online.js no esta cargado` en la consola del juego: falta el `<script src="online.js">` antes de `index.js`.
- Error de link por `CL_Disconnect` / `M_ForceMenuOff`: tienen que ser simbolos no-static del cliente (en este fork lo son).
