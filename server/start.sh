#!/bin/sh
# El proxy arranca la sala principal y crea/cierra las demás salas (un q2ded por sala).
# pak0.pak solo trae los mapas de la campaña: las salas son COOP (max. 4 jugadores).
# Para deathmatch con q2dm1..8 agregá pak1.pak a /q2/baseq2 y poné Q2_DM=1 (y Q2_MODE=dm Q2_MAP=q2dm1 para la principal).
cd /q2
exec node /app/proxy.js
