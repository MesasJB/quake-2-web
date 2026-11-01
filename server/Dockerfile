# Se construye desde la RAIZ del repositorio:  docker build -f server/Dockerfile .
FROM ubuntu:24.04 AS build
RUN apt-get update && apt-get install -y --no-install-recommends build-essential && rm -rf /var/lib/apt/lists/*
COPY Qwasm2-main /src
WORKDIR /src
RUN make server game WITH_OPENAL=no WITH_CURL=no -j"$(nproc)"

FROM ubuntu:24.04
RUN apt-get update && apt-get install -y --no-install-recommends nodejs && rm -rf /var/lib/apt/lists/*
WORKDIR /q2
COPY --from=build /src/release/q2ded /q2/q2ded
COPY --from=build /src/release/baseq2/game.so /q2/baseq2/game.so
COPY baseq2/pak0.part* /tmp/
RUN cat /tmp/pak0.part0 /tmp/pak0.part1 /tmp/pak0.part2 /tmp/pak0.part3 > /q2/baseq2/pak0.pak && rm /tmp/pak0.part*
COPY server/proxy.js /app/proxy.js
COPY server/start.sh /app/start.sh
RUN chmod +x /app/start.sh && useradd -m q2 && chown -R q2:q2 /q2
USER q2
ENV PORT=8080
EXPOSE 8080
CMD ["/app/start.sh"]
