# Relay online de Quake 2 web. Se construye desde la RAIZ del repositorio:
#   docker build -f server/Dockerfile .
# Ya no hace falta compilar q2ded ni subir pak0: el juego corre en el navegador del host.
FROM node:22-alpine
WORKDIR /app
COPY server/server.js /app/server.js
# STATIC_ROOT apunta a una carpeta vacia: este contenedor es solo relay (la pagina va en GitHub Pages)
ENV PORT=8080 STATIC_ROOT=/nada
EXPOSE 8080
USER node
CMD ["node", "/app/server.js"]
