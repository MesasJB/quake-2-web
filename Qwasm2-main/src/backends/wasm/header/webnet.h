/*
 * Quake 2 WebAssembly: capa de red "Web" (misma estructura que net_web.c del Quake 1 web).
 *
 * Los navegadores no pueden usar UDP, asi que el multijugador va por un servidor relay
 * (server/server.js) a traves de un WebSocket que maneja online.js. Este modulo reemplaza
 * a backends/unix/network.c en la build de Emscripten e implementa la API NET_* del motor.
 */

#ifndef WASM_WEBNET_H
#define WASM_WEBNET_H

/* Se llama una vez por frame (desde wasm_poll_commands): paquetes, sala, comandos de la pagina. */
void WebNet_Tick(void);

#endif
