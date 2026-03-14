# ── Build stage ──────────────────────────────────────────────────────────────
FROM debian:bookworm AS build

RUN apt-get update -qq && \
    apt-get install -y --no-install-recommends \
        g++ cmake ninja-build libboost-all-dev && \
    rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . .

RUN cmake --preset linux-release && \
    cmake --build --preset linux-release

# ── Runtime stage ─────────────────────────────────────────────────────────────
FROM debian:bookworm-slim AS runtime

RUN apt-get update -qq && \
    apt-get install -y --no-install-recommends libboost-program-options && \
    rm -rf /var/lib/apt/lists/* && \
    useradd --system --no-create-home minilog

COPY --from=build /src/build/linux-release/minilog /usr/local/bin/minilog

USER minilog

EXPOSE 514/udp

# Config file must be provided via a bind mount at /etc/minilog/syslog-server.conf
CMD ["/usr/local/bin/minilog", "/etc/minilog/syslog-server.conf"]
