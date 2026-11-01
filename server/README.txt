SERVIDOR ONLINE DE QUAKE 2 WEB (salas)

Un solo contenedor Docker con:
 - proxy.js : WebSocket <-> UDP y administrador de salas (cada sala = un servidor dedicado q2ded)
 - q2ded    : compilado desde Qwasm2-main

Render (gratis)
 1. render.com -> New -> Web Service -> tu repositorio de GitHub.
 2. Runtime: Docker. Dockerfile Path: server/Dockerfile. Build context: . (la raíz). Instance type: Free.
 3. Cuando diga "Live", abrí https://TU-APP.onrender.com/rooms: tiene que mostrar la "Sala principal".
 4. En el juego: Multiplayer -> ventanita -> registrate y pegá wss://TU-APP.onrender.com (o usá el panel "Servidor").

Salas
 - Sala principal: siempre abierta (coop base1).
 - Los jugadores crean más salas desde la ventanita de Multiplayer (máx. 3 extra, de hasta 4 jugadores, coop).
 - Las salas creadas se cierran solas a los 10 min sin jugadores.
 - Variables opcionales: MAX_ROOMS, ROOM_IDLE_MS, Q2_MAP. Deathmatch (q2dm1..8) requiere pak1.pak y Q2_DM=1.
 - Plan gratis de Render: 512 MB de RAM y se duerme tras ~15 min sin tráfico (el primero espera ~1 min).
