# ── Build stage ──────────────────────────────────────────────────────────────
# Only the C++ syslog server is built here.  The Go web-viewer and the Python
# cli-viewer are not included — this image is intended for headless syslog
# collection.  Use the viewers on the host or in a separate container.
FROM debian:trixie AS build

RUN apt-get update -qq && \
    apt-get install -y --no-install-recommends \
        g++ cmake ninja-build libboost-all-dev && \
    rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . .

RUN cmake --preset linux-docker && \
    cmake --build --preset linux-docker --target minilog

# ── Runtime stage ─────────────────────────────────────────────────────────────
FROM debian:trixie-slim AS runtime

RUN useradd --system --no-create-home minilog

COPY --from=build /src/build/linux-docker/bin/minilog /usr/local/bin/minilog

USER minilog

EXPOSE 514/udp

# Config file must be provided via a bind mount at /etc/minilog/minilog.conf
CMD ["/usr/local/bin/minilog", "/etc/minilog/minilog.conf"]
