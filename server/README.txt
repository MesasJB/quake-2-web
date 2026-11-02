SERVIDOR ONLINE DE QUAKE 2 WEB (relay de salas)

Es el mismo relay del Quake 1 web (mismo protocolo). NO ejecuta el juego: el que crea una
sala (Multiplayer > start network server) corre el servidor en su navegador, y este relay
solo anuncia las salas y reenvia paquetes entre los jugadores de cada sala.

Local (juego + online en el mismo servidor)
  node server/server.js          (desde la carpeta que tiene index.html y baseq2)
  abri http://localhost:8765

Render (gratis), solo relay
 1. render.com -> New -> Web Service -> tu repositorio de GitHub.
 2. Runtime: Docker. Dockerfile Path: server/Dockerfile. Build context: . (la raiz). Instance: Free.
 3. Cuando diga "Live", abri https://TU-APP.onrender.com/ : tiene que decir "Quake online OK".
 4. Si tu URL no es quake-2-web.onrender.com, cambiala en index.html (window.Q2_DEFAULT_SERVER)
    o pegala en el panel "Servidor" de la pagina.
 - Plan gratis: se duerme tras ~15 min sin trafico (la primera conexion espera ~1 min).

Variables opcionales: PORT, HOST, STATIC_ROOT, MAX_CLIENTS (500), MAX_PER_IP (40).

Archivos viejos que ya no se usan (podes borrarlos): server/proxy.js, server/start.sh
