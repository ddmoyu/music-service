# syntax=docker/dockerfile:1
# Minimal Linux image for music-service. Build from the repository root:
#   docker build -f packaging/docker/Dockerfile -t music-service .
# The base is pinned by digest (ubuntu:24.04 image index, looked up 2026-10-20);
# glibc 2.39 in both stages, so the program runs on the libraries it was built with.
ARG BASE=ubuntu:24.04@sha256:68ee22004a9109b44973313b02ff7b1de250f1de0f5ecb628d73f579aaff33ac

FROM ${BASE} AS build
ARG BUILD_JOBS=3
RUN apt-get update \
 && apt-get install -y --no-install-recommends g++ git ca-certificates python3-pip \
 && rm -rf /var/lib/apt/lists/*
# Ubuntu 24.04 ships CMake 3.28; the project needs 3.30 or later.
RUN python3 -m pip install --break-system-packages --no-cache-dir cmake==3.31.6 ninja==1.13.0
WORKDIR /src
COPY CMakeLists.txt CMakePresets.json ./
COPY cmake cmake
COPY src src
COPY packaging packaging
COPY docs/api docs/api
COPY docs/frontend-integration.md docs/frontend-integration.md
COPY examples/frontend examples/frontend
RUN cmake --preset linux-gcc-release \
 && cmake --build --preset linux-gcc-release --target music_service_server --parallel ${BUILD_JOBS} \
 && cmake --install out/build/linux-gcc-release --prefix /opt/music-service --strip

FROM ${BASE}
# Only the program, its runtime libraries (in the base) and CA certificates for
# upstream HTTPS. Runs as an unprivileged user; state lives on volumes.
RUN apt-get update \
 && apt-get install -y --no-install-recommends ca-certificates \
 && rm -rf /var/lib/apt/lists/* \
 && useradd --system --uid 10001 --no-create-home --home-dir /nonexistent --shell /usr/sbin/nologin music-service \
 && mkdir -p /data /downloads /etc/music-service \
 && chown music-service:music-service /data /downloads
COPY --from=build /opt/music-service/bin/music-service /usr/local/bin/music-service
COPY --from=build /opt/music-service/share/music-service /usr/local/share/music-service
COPY packaging/docker/config.json /etc/music-service/config.json
USER music-service
VOLUME ["/data", "/downloads"]
EXPOSE 17632 17633 17634
# exec form: the service is PID 1 and receives SIGTERM from docker stop directly.
ENTRYPOINT ["/usr/local/bin/music-service", "--config", "/etc/music-service/config.json"]
CMD ["serve"]
HEALTHCHECK --interval=15s --timeout=5s --start-period=5s --retries=3 \
  CMD ["/usr/local/bin/music-service", "--config", "/etc/music-service/config.json", "healthcheck"]
