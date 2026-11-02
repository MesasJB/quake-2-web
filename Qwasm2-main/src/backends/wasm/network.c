/*
 * network.c -- capa de red para navegador (reemplaza a backends/unix/network.c en Emscripten).
 *
 * Es el equivalente en Quake 2 del driver net_web.c del Quake 1 web, con la misma estructura:
 *
 *   motor (este archivo)  <->  online.js (WebSocket)  <->  server/server.js (relay + salas)
 *
 * Quien crea una partida multijugador (start network server) es el "host": su navegador corre
 * el servidor y se anuncia como sala. Los demas se unen con "connect #12". El relay solo
 * reenvia paquetes entre jugadores que hicieron HELLO entre si; no ejecuta el juego.
 *
 * Quake 2 ya trae su propio protocolo de datagramas (netchan) con reintentos y orden, y el
 * WebSocket ademas es confiable, asi que cada datagrama del motor viaja tal cual.
 *
 * Formato de cada paquete con el relay:   [id u32 big-endian][tipo u8][datos...]
 * (id = destino al enviar, origen al recibir)
 *
 * Direcciones: cada jugador tiene un id numerico. Se escribe "#12" (o solo "12").
 * Internamente es un netadr_t NA_IP con el id en ip[0..3] y puerto fijo.
 */

#include "../../common/header/common.h"
#include "../../server/header/server.h"
#include "header/capmouse.h"
#include "header/webnet.h"

#include <emscripten.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define T_HELLO         1
#define T_ACCEPT        2
#define T_REJECT        3
#define T_RELIABLE      4
#define T_UNRELIABLE    5
#define T_CLOSE         6

#define WEB_MAXPEERS    32
#define WEB_QUEUE       64      /* paquetes en cola por lado (potencia de 2) */
#define WEB_FRAMESIZE   (MAX_MSGLEN + 64)
#define WEB_MAXSEEN     64
#define WEB_PEER_IDLE   120000  /* ms sin trafico: se libera el vinculo */

#define MAX_LOOPBACK    4

netadr_t net_local_adr;

/*============================================================================
  Puente con JavaScript (online.js). Se definen con EM_JS (fuera de cualquier
  funcion con setjmp, que es lo que Emscripten exige). No dependen de helpers
  del runtime: solo de HEAPU8.
============================================================================*/
EM_JS(int, js_net_start, (void), {
	if (typeof QOnline === 'undefined') return 0;
	QOnline.start();
	return 1;
});

EM_JS(int, js_net_recv, (unsigned char *buf, int max), {
	var p = QOnline.shift();
	if (!p) return 0;
	if (p.length > max) return -1;
	HEAPU8.set(p, buf);
	return p.length;
});

EM_JS(int, js_net_send, (unsigned char *buf, int len, int unreliable), {
	return QOnline.send(HEAPU8.slice(buf, buf + len), unreliable);
});

EM_JS(int, js_net_myid, (void), { return QOnline.myId(); });
EM_JS(int, js_net_epoch, (void), { return QOnline.epoch(); });
EM_JS(void, js_net_listen, (int on), { QOnline.setListen(on); });
EM_JS(void, js_net_refresh, (void), { QOnline.refreshRooms(); });

EM_JS(void, js_net_room, (const char *name, const char *map, int players, int max), {
	function cstr (p) { var s = ''; for (var c; (c = HEAPU8[p++]); ) s += String.fromCharCode(c); return s; }
	QOnline.setRoom(cstr(name), cstr(map), players, max);
});

EM_JS(void, js_net_notify, (const char *msg), {
	var s = ''; for (var c; (c = HEAPU8[msg++]); ) s += String.fromCharCode(c);
	QOnline.notify(s);
});

EM_JS(int, js_net_cmd, (char *buf, int max), {
	var s = QOnline.pollCmd();
	if (s === null) return 0;
	var e = new TextEncoder().encode(s);
	var n = Math.min(e.length, max - 1);
	HEAPU8.set(e.subarray(0, n), buf);
	HEAPU8[buf + n] = 0;
	return n;
});

EM_JS(int, js_net_room_count, (void), { return QOnline.roomCount(); });

EM_JS(int, js_net_room_line, (int i, char *buf, int max), {
	var s = QOnline.roomLine(i);
	var e = new TextEncoder().encode(s);
	var n = Math.min(e.length, max - 1);
	HEAPU8.set(e.subarray(0, n), buf);
	HEAPU8[buf + n] = 0;
	return n;
});

/*============================================================================
  Estado
============================================================================*/

/* Loopback: el cliente local habla con el servidor local sin pasar por la red */
typedef struct
{
	byte data[MAX_MSGLEN];
	int datalen;
} loopmsg_t;

typedef struct
{
	loopmsg_t msgs[MAX_LOOPBACK];
	int get, send;
} loopback_t;

static loopback_t loopbacks[2];

/* Paquetes recibidos del relay, ya repartidos: [NS_CLIENT] y [NS_SERVER] */
typedef struct
{
	unsigned int id;        /* de quien vino */
	int len;
	byte data[MAX_MSGLEN];
} webpkt_t;

typedef struct
{
	webpkt_t pkts[WEB_QUEUE];
	unsigned int get, put;
} webqueue_t;

static webqueue_t queues[2];

typedef struct
{
	qboolean used;
	unsigned int id;        /* id del otro jugador */
	qboolean hostside;      /* true: alguien se unio a mi sala */
	int last_ms;            /* ultimo trafico */
} webpeer_t;

static webpeer_t peers[WEB_MAXPEERS];
static qboolean web_ready = false;
static qboolean web_listening = false;
static int epoch_seen = 0;
static unsigned int shown_id = 0xffffffff;
static int next_room_update = 0;
static int next_sweep = 0;
static byte rxframe[WEB_FRAMESIZE];
static byte txframe[WEB_FRAMESIZE];

/* Cortes de conexion detectados dentro de NET_GetPacket: se resuelven en WebNet_Tick */
static qboolean pending_disconnect = false;
static char pending_reason[96];

/* Busqueda de salas ("info" por broadcast -> se contesta con la lista del relay) */
static int scan_until = 0;
static unsigned int scan_seen[WEB_MAXSEEN];
static int scan_nseen = 0;

extern void M_ForceMenuOff(void);   /* client/menu/menu.c */
extern void CL_Disconnect(void);    /* client/cl_network.c */

/*============================================================================
  Utilidades
============================================================================*/
static void
Web_IdToAdr(unsigned int id, netadr_t *a)
{
	memset(a, 0, sizeof(*a));
	a->type = NA_IP;
	a->ip[0] = (id >> 24) & 0xff;
	a->ip[1] = (id >> 16) & 0xff;
	a->ip[2] = (id >> 8) & 0xff;
	a->ip[3] = id & 0xff;
	a->port = BigShort(PORT_SERVER);
}

static unsigned int
Web_AdrToId(const netadr_t *a)
{
	return ((unsigned int)a->ip[0] << 24) | ((unsigned int)a->ip[1] << 16) |
		   ((unsigned int)a->ip[2] << 8) | (unsigned int)a->ip[3];
}

/* acepta "#12", "12" o "#12:27910" */
static qboolean
Web_ParseId(const char *s, unsigned int *id)
{
	unsigned long v = 0;
	int digits = 0;

	if (*s == '#')
	{
		s++;
	}

	while (*s >= '0' && *s <= '9' && digits < 9)
	{
		v = v * 10 + (*s - '0');
		s++;
		digits++;
	}

	if (!digits || v == 0)
	{
		return false;
	}

	if (*s && *s != ':')
	{
		return false;
	}

	*id = (unsigned int)v;
	return true;
}

static int
Web_SendFrame(unsigned int id, int type, const byte *payload, int len, int unreliable)
{
	if (len < 0 || len > MAX_MSGLEN)
	{
		return 0;
	}

	txframe[0] = (id >> 24) & 0xff;
	txframe[1] = (id >> 16) & 0xff;
	txframe[2] = (id >> 8) & 0xff;
	txframe[3] = id & 0xff;
	txframe[4] = type;

	if (len)
	{
		memcpy(txframe + 5, payload, len);
	}

	return js_net_send(txframe, len + 5, unreliable);
}

static webpeer_t *
Web_FindPeer(unsigned int id)
{
	int i;

	for (i = 0; i < WEB_MAXPEERS; i++)
	{
		if (peers[i].used && peers[i].id == id)
		{
			return &peers[i];
		}
	}

	return NULL;
}

static webpeer_t *
Web_NewPeer(unsigned int id, qboolean hostside)
{
	int i;

	for (i = 0; i < WEB_MAXPEERS; i++)
	{
		if (!peers[i].used)
		{
			memset(&peers[i], 0, sizeof(webpeer_t));
			peers[i].used = true;
			peers[i].id = id;
			peers[i].hostside = hostside;
			peers[i].last_ms = Sys_Milliseconds();
			return &peers[i];
		}
	}

	return NULL;
}

/* Avisa al otro lado (si hace falta) y libera el vinculo */
static void
Web_ClosePeer(webpeer_t *p, qboolean notify_other)
{
	if (notify_other)
	{
		Web_SendFrame(p->id, T_CLOSE, NULL, 0, 0);
	}

	memset(p, 0, sizeof(webpeer_t));
}

static void
Web_CloseAllPeers(void)
{
	int i;

	for (i = 0; i < WEB_MAXPEERS; i++)
	{
		if (peers[i].used)
		{
			Web_ClosePeer(&peers[i], true);
		}
	}
}

static void
Web_Reject(unsigned int id, const char *reason)
{
	Web_SendFrame(id, T_REJECT, (const byte *)reason, (int)strlen(reason), 0);
}

static void
Web_Push(webqueue_t *q, unsigned int id, const byte *data, int len)
{
	webpkt_t *pkt;

	if (len <= 0 || len > MAX_MSGLEN)
	{
		return;
	}

	if (q->put - q->get >= WEB_QUEUE)
	{
		q->get++;   /* el motor no esta leyendo (ej. cargando un mapa): descartamos el mas viejo */
	}

	pkt = &q->pkts[q->put & (WEB_QUEUE - 1)];
	q->put++;
	pkt->id = id;
	pkt->len = len;
	memcpy(pkt->data, data, len);
}

static void
Web_RequestDisconnect(const char *reason)
{
	pending_disconnect = true;
	Q_strlcpy(pending_reason, reason, sizeof(pending_reason));
}

/*============================================================================
  Recibe todo lo que llego por el WebSocket y lo reparte
============================================================================*/
static void
Web_Pump(void)
{
	int n, iter, type, len;
	unsigned int id;
	webpeer_t *p;
	char reason[96];

	if (!web_ready)
	{
		return;
	}

	for (iter = 0; iter < 512; iter++)
	{
		n = js_net_recv(rxframe, sizeof(rxframe));

		if (n == 0)
		{
			break;
		}

		if (n < 5)
		{
			continue;
		}

		id = ((unsigned int)rxframe[0] << 24) | ((unsigned int)rxframe[1] << 16) |
			 ((unsigned int)rxframe[2] << 8) | rxframe[3];
		type = rxframe[4];
		len = n - 5;

		switch (type)
		{
			case T_HELLO:       /* alguien quiere entrar a mi sala */
				p = Web_FindPeer(id);

				if (p)
				{
					p->last_ms = Sys_Milliseconds();
					break;
				}

				if (!web_listening)
				{
					Web_Reject(id, "La sala no esta abierta.");
					break;
				}

				if (!Web_NewPeer(id, true))
				{
					Web_Reject(id, "Sala llena.");
					break;
				}

				Web_SendFrame(id, T_ACCEPT, NULL, 0, 0);
				break;

			case T_ACCEPT:
				break;

			case T_REJECT:
				p = Web_FindPeer(id);

				if (p)
				{
					if (len > (int)sizeof(reason) - 1)
					{
						len = sizeof(reason) - 1;
					}

					memcpy(reason, rxframe + 5, len);
					reason[len] = 0;

					/* El motivo se muestra una sola vez (consola + aviso de la pagina)
					   cuando WebNet_Tick corta el intento de conexion */
					if (!p->hostside)
					{
						Web_RequestDisconnect(reason);
					}

					memset(p, 0, sizeof(webpeer_t));
				}

				break;

			case T_RELIABLE:
			case T_UNRELIABLE:
				p = Web_FindPeer(id);

				if (p)
				{
					p->last_ms = Sys_Milliseconds();
					Web_Push(&queues[p->hostside ? NS_SERVER : NS_CLIENT],
							id, rxframe + 5, len);
				}

				break;

			case T_CLOSE:
				p = Web_FindPeer(id);

				if (p)
				{
					if (!p->hostside)
					{
						Web_RequestDisconnect("El host cerro la sala.");
					}

					memset(p, 0, sizeof(webpeer_t));
				}

				break;
		}
	}
}

/*============================================================================
  Lista de salas (para "search for local servers" del menu)
============================================================================*/
static void
Web_ScanRooms(void)
{
	int i, j, k, count, seen;
	unsigned int rid;
	char line[160], *f[5], *s;
	char info[64], name[17], map[9];
	byte pkt[96];
	int len;

	count = js_net_room_count();

	for (i = 0; i < count; i++)
	{
		if (js_net_room_line(i, line, sizeof(line)) <= 0)
		{
			continue;
		}

		/* id|nombre|mapa|jugadores|maximo */
		f[0] = line;
		k = 1;

		for (s = line; *s && k < 5; s++)
		{
			if (*s == '|')
			{
				*s = 0;
				f[k++] = s + 1;
			}
		}

		if (k < 5)
		{
			continue;
		}

		rid = (unsigned int)atoi(f[0]);

		if (!rid || rid == (unsigned int)js_net_myid())
		{
			continue;
		}

		seen = 0;

		for (j = 0; j < scan_nseen; j++)
		{
			if (scan_seen[j] == rid)
			{
				seen = 1;
			}
		}

		if (seen || scan_nseen >= WEB_MAXSEEN)
		{
			continue;
		}

		scan_seen[scan_nseen++] = rid;

		Q_strlcpy(name, f[1], sizeof(name));
		Q_strlcpy(map, f[2], sizeof(map));
		Com_sprintf(info, sizeof(info), "%16s %8s %2i/%2i\n", name, map, atoi(f[3]), atoi(f[4]));

		/* Mismo paquete que contesta un servidor real a "info": -1 "info\n" + texto */
		pkt[0] = pkt[1] = pkt[2] = pkt[3] = 0xff;
		len = 4;
		memcpy(pkt + len, "info\n", 5);
		len += 5;
		memcpy(pkt + len, info, strlen(info));
		len += (int)strlen(info);

		Web_Push(&queues[NS_CLIENT], rid, pkt, len);
	}
}

/*============================================================================
  API NET_* del motor
============================================================================*/
void
NET_Init(void)
{
	memset(peers, 0, sizeof(peers));
	memset(queues, 0, sizeof(queues));
	memset(loopbacks, 0, sizeof(loopbacks));
	memset(&net_local_adr, 0, sizeof(net_local_adr));   /* NA_LOOPBACK */

	if (!js_net_start())
	{
		Com_Printf("Web network: online.js no esta cargado\n");
		return;
	}

	epoch_seen = js_net_epoch();
	web_ready = true;
	Com_Printf("Web network initialized\n");
}

void
NET_Shutdown(void)
{
	NET_Config(false);
	web_ready = false;
}

/*
 * Un juego de un solo jugador solo usa loopback. Al cerrar la red, se cortan
 * todos los vinculos con otros jugadores.
 */
void
NET_Config(qboolean multiplayer)
{
	if (!multiplayer)
	{
		if (web_ready)
		{
			Web_CloseAllPeers();
		}

		memset(queues, 0, sizeof(queues));
	}
}

qboolean
NET_CompareAdr(netadr_t a, netadr_t b)
{
	if (a.type != b.type)
	{
		return false;
	}

	if (a.type == NA_LOOPBACK)
	{
		return true;
	}

	if (a.type == NA_IP)
	{
		return (a.ip[0] == b.ip[0]) && (a.ip[1] == b.ip[1]) &&
			   (a.ip[2] == b.ip[2]) && (a.ip[3] == b.ip[3]) && (a.port == b.port);
	}

	return false;
}

/* Compara sin el puerto */
qboolean
NET_CompareBaseAdr(netadr_t a, netadr_t b)
{
	if (a.type != b.type)
	{
		return false;
	}

	if (a.type == NA_LOOPBACK)
	{
		return true;
	}

	if (a.type == NA_IP)
	{
		return (a.ip[0] == b.ip[0]) && (a.ip[1] == b.ip[1]) &&
			   (a.ip[2] == b.ip[2]) && (a.ip[3] == b.ip[3]);
	}

	return false;
}

qboolean
NET_IsLocalAddress(netadr_t adr)
{
	return NET_CompareAdr(adr, net_local_adr);
}

char *
NET_BaseAdrToString(netadr_t a)
{
	static char s[64];

	if (a.type == NA_LOOPBACK)
	{
		Com_sprintf(s, sizeof(s), "loopback");
	}
	else if (a.type == NA_IP)
	{
		Com_sprintf(s, sizeof(s), "#%u", Web_AdrToId(&a));
	}
	else
	{
		Com_sprintf(s, sizeof(s), "unknown");
	}

	return s;
}

char *
NET_AdrToString(netadr_t a)
{
	return NET_BaseAdrToString(a);
}

/*
 * "localhost" es el servidor propio (loopback); "#12" / "12" es otro jugador
 */
qboolean
NET_StringToAdr(const char *s, netadr_t *a)
{
	unsigned int id;

	memset(a, 0, sizeof(*a));

	if (!strcmp(s, "localhost"))
	{
		a->type = NA_LOOPBACK;
		return true;
	}

	if (!Web_ParseId(s, &id))
	{
		return false;
	}

	Web_IdToAdr(id, a);
	return true;
}

static qboolean
NET_GetLoopPacket(netsrc_t sock, netadr_t *net_from, sizebuf_t *net_message)
{
	int i;
	loopback_t *loop;

	loop = &loopbacks[sock];

	if (loop->send - loop->get > MAX_LOOPBACK)
	{
		loop->get = loop->send - MAX_LOOPBACK;
	}

	if (loop->get >= loop->send)
	{
		return false;
	}

	i = loop->get & (MAX_LOOPBACK - 1);
	loop->get++;

	memcpy(net_message->data, loop->msgs[i].data, loop->msgs[i].datalen);
	net_message->cursize = loop->msgs[i].datalen;
	*net_from = net_local_adr;
	return true;
}

static void
NET_SendLoopPacket(netsrc_t sock, int length, void *data, netadr_t to)
{
	int i;
	loopback_t *loop;

	loop = &loopbacks[sock ^ 1];

	i = loop->send & (MAX_LOOPBACK - 1);
	loop->send++;

	memcpy(loop->msgs[i].data, data, length);
	loop->msgs[i].datalen = length;
}

qboolean
NET_GetPacket(netsrc_t sock, netadr_t *net_from, sizebuf_t *net_message)
{
	webqueue_t *q;
	webpkt_t *pkt;

	if (NET_GetLoopPacket(sock, net_from, net_message))
	{
		return true;
	}

	if (!web_ready)
	{
		return false;
	}

	Web_Pump();

	q = &queues[sock];

	while (q->get < q->put)
	{
		pkt = &q->pkts[q->get & (WEB_QUEUE - 1)];
		q->get++;

		Web_IdToAdr(pkt->id, net_from);

		if (pkt->len > net_message->maxsize)
		{
			Com_Printf("Oversize packet from %s\n", NET_AdrToString(*net_from));
			continue;
		}

		memcpy(net_message->data, pkt->data, pkt->len);
		net_message->cursize = pkt->len;
		return true;
	}

	return false;
}

void
NET_SendPacket(netsrc_t sock, int length, void *data, netadr_t to)
{
	unsigned int id;
	webpeer_t *p;
	const byte *b = (const byte *)data;
	int oob, i;

	switch (to.type)
	{
		case NA_LOOPBACK:
			NET_SendLoopPacket(sock, length, data, to);
			return;

		case NA_BROADCAST:
			/* "search for local servers": contestamos con las salas del relay */
			if (web_ready && length >= 8 &&
				b[0] == 0xff && b[1] == 0xff && b[2] == 0xff && b[3] == 0xff &&
				!memcmp(b + 4, "info", 4))
			{
				scan_nseen = 0;
				scan_until = Sys_Milliseconds() + 2500;
				js_net_refresh();
				Web_ScanRooms();
			}

			return;

		case NA_IP:
			break;

		default:
			return;
	}

	if (!web_ready || length <= 0 || length > MAX_MSGLEN)
	{
		return;
	}

	id = Web_AdrToId(&to);
	p = Web_FindPeer(id);

	if (!p)
	{
		/* El host no inicia vinculos: sus jugadores aparecen con HELLO */
		if (sock == NS_SERVER)
		{
			return;
		}

		if (!js_net_myid())
		{
			Com_Printf("Sin conexion con el servidor online.\n");
			js_net_notify("Sin conexion con el servidor online.");
			return;
		}

		if (id == (unsigned int)js_net_myid())
		{
			Com_Printf("No podes unirte a tu propia sala.\n");
			js_net_notify("No podes unirte a tu propia sala.");
			return;
		}

		/* un cliente habla con una sola sala a la vez: soltamos las anteriores */
		for (i = 0; i < WEB_MAXPEERS; i++)
		{
			if (peers[i].used && !peers[i].hostside)
			{
				Web_ClosePeer(&peers[i], true);
			}
		}

		p = Web_NewPeer(id, false);

		if (!p)
		{
			return;
		}

		Web_SendFrame(id, T_HELLO, NULL, 0, 0);
	}

	p->last_ms = Sys_Milliseconds();

	/* Los paquetes de conexion (-1 al principio) no se descartan nunca;
	   los de juego si el WebSocket esta saturado (mejor perder uno que acumular lag) */
	oob = (length >= 4 && b[0] == 0xff && b[1] == 0xff && b[2] == 0xff && b[3] == 0xff);
	Web_SendFrame(id, oob ? T_RELIABLE : T_UNRELIABLE, b, length, !oob);
}

/* En el navegador el bucle principal lo maneja el navegador: no hay nada que dormir */
void
NET_Sleep(int msec)
{
}

/*============================================================================
  Una vez por frame: estado de la conexion, sala, comandos de la pagina
============================================================================*/
static void
Web_RunCommand(char *line)
{
	char *s;

	/* Solo aceptamos los comandos que usa la pagina */
	if (!strncmp(line, "name ", 5))
	{
		Cbuf_AddText(va("%s\n", line));
	}
	else if (!strncmp(line, "join ", 5))
	{
		for (s = line + 5; *s; s++)
		{
			if (*s < '0' || *s > '9')
			{
				return;
			}
		}

		/* Cierra el menu, captura el mouse y entra a la sala */
		M_ForceMenuOff();
		wasm_capture_mouse();
		Cbuf_AddText(va("connect \"#%s\"\n", line + 5));
	}
}

static void
Web_AnnounceRoom(void)
{
	char hostname[32];
	int i, players = 0, maxc = 0;

	if (maxclients)
	{
		maxc = (int)maxclients->value;
	}

	if (svs.initialized && svs.clients)
	{
		for (i = 0; i < maxc; i++)
		{
			if (svs.clients[i].state >= cs_connected)
			{
				players++;
			}
		}
	}

	Q_strlcpy(hostname, Cvar_VariableString("hostname"), sizeof(hostname));

	if (!hostname[0] || !strcmp(hostname, "noname"))
	{
		Com_sprintf(hostname, sizeof(hostname), "Sala %s", Cvar_VariableString("name"));
		hostname[16] = 0;
		Cvar_Set("hostname", hostname);
	}

	js_net_room(hostname, sv.name, players, maxc);
}

void
WebNet_Tick(void)
{
	char line[128];
	int i, n, ep, now;
	unsigned int id;
	qboolean listening;

	if (!web_ready)
	{
		return;
	}

	Web_Pump();
	now = Sys_Milliseconds();

	/* Se corto (o reconecto) el WebSocket: todas las conexiones se perdieron */
	ep = js_net_epoch();

	if (ep != epoch_seen)
	{
		epoch_seen = ep;

		for (i = 0; i < WEB_MAXPEERS; i++)
		{
			if (peers[i].used)
			{
				if (!peers[i].hostside)
				{
					Web_RequestDisconnect("Se perdio la conexion con el servidor online.");
				}

				memset(&peers[i], 0, sizeof(webpeer_t));
			}
		}

		memset(queues, 0, sizeof(queues));
		next_room_update = 0;
	}

	/* Cortes detectados mientras se leian paquetes */
	if (pending_disconnect)
	{
		pending_disconnect = false;
		Com_Printf("%s\n", pending_reason);
		js_net_notify(pending_reason);
		CL_Disconnect();
	}

	/* Direccion propia (se avisa una vez, la pagina la muestra siempre) */
	id = (unsigned int)js_net_myid();

	if (id != shown_id)
	{
		shown_id = id;

		if (id)
		{
			Com_Printf("Tu direccion online es #%u\n", id);
		}
	}

	/* Tengo un servidor multijugador abierto? Entonces soy una sala */
	listening = (Com_ServerState() != 0 && maxclients && maxclients->value > 1);

	if (listening != web_listening)
	{
		web_listening = listening;
		js_net_listen(listening);
		next_room_update = 0;   /* anunciar la sala ya mismo */
	}

	if (web_listening && now >= next_room_update)
	{
		next_room_update = now + 2000;
		Web_AnnounceRoom();
	}

	/* Mientras dura una busqueda, vamos sumando las salas que llegan */
	if (now < scan_until)
	{
		Web_ScanRooms();
	}

	/* Soltar vinculos viejos (libera cupo en el relay) */
	if (now >= next_sweep)
	{
		next_sweep = now + 5000;

		for (i = 0; i < WEB_MAXPEERS; i++)
		{
			if (peers[i].used && now - peers[i].last_ms > WEB_PEER_IDLE)
			{
				Web_ClosePeer(&peers[i], true);
			}
		}
	}

	/* Comandos que manda la pagina (cambiar nombre, unirse a una sala) */
	for (i = 0; i < 8; i++)
	{
		n = js_net_cmd(line, sizeof(line));

		if (n <= 0)
		{
			break;
		}

		Web_RunCommand(line);
	}
}
