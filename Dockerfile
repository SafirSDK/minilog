# ── Build stage ──────────────────────────────────────────────────────────────
FROM debian:trixie AS build

RUN apt-get update -qq && \
    apt-get install -y --no-install-recommends \
        g++ cmake ninja-build libboost-all-dev && \
    rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . .

RUN cmake --preset linux-docker && \
    cmake --build --preset linux-docker

# ── Runtime stage ─────────────────────────────────────────────────────────────
FROM debian:trixie-slim AS runtime

RUN useradd --system --no-create-home minilog

COPY --from=build /src/build/linux-docker/minilog /usr/local/bin/minilog

USER minilog

EXPOSE 514/udp

# Config file must be provided via a bind mount at /etc/minilog/syslog-server.conf
CMD ["/usr/local/bin/minilog", "/etc/minilog/syslog-server.conf"]
