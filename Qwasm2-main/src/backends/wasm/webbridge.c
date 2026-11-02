/*
 * Quake 2 WebAssembly <-> pagina web: puente de menu y comandos.
 *
 * OJO: dentro de EM_ASM no puede haber comas "sueltas" (solo dentro de parentesis)
 * ni comillas simples vacias, porque el preprocesador de C las toma como argumentos.
 */

#include "header/webbridge.h"
#include "header/webnet.h"
#include "../../common/header/common.h"

#include <emscripten.h>

void wasm_menu_event(int id)
{
	EM_ASM({
		if (typeof Module.onMenuEvent === "function")
			Module.onMenuEvent($0);
	}, id);
}

void wasm_poll_commands(void)
{
	static char buf[256];

	/* Red online: paquetes, estado de la sala y comandos de online.js (una vez por frame) */
	WebNet_Tick();

	/* Copia (solo ASCII imprimible) el comando pendiente de la pagina a buf */
	int n = EM_ASM_INT({
		var c = Module.pendingCmd;
		if (!c) return 0;
		Module.pendingCmd = "";
		c = String(c).replace(/[^ -~]/g, " ").slice(0, $1 - 1);
		stringToUTF8(c, $0, $1);
		return c.length;
	}, buf, (int)sizeof(buf));

	if (n > 0)
	{
		Cbuf_AddText(buf);
		Cbuf_AddText("\n");
	}
}
