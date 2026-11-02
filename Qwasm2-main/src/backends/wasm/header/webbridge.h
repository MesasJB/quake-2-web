/*
 * Quake 2 WebAssembly <-> pagina web: puente de menu y comandos.
 *
 * wasm_menu_event(id)   avisa a la pagina (Module.onMenuEvent(id)) que se abrio un menu.
 *                       id 2 = join network server, id 3 = start network server.
 * wasm_poll_commands()  una vez por frame: corre la red online (WebNet_Tick, ver network.c) y
 *                       ejecuta en el motor el comando que la pagina dejo en Module.pendingCmd.
 */

#ifndef WASM_WEBBRIDGE_H
#define WASM_WEBBRIDGE_H

void wasm_menu_event(int id);
void wasm_poll_commands(void);

#endif
