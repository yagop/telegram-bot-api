FROM debian:13-slim AS build

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        g++ make cmake git gperf libssl-dev zlib1g-dev ccache ninja-build \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . .

ENV CCACHE_DIR=/ccache
RUN --mount=type=cache,target=/ccache \
    cmake -S . -B build -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX=/usr/local \
    && cmake --build build --target install \
    && ccache -s

FROM debian:13-slim

# openssl guarantees libssl/libcrypto whatever the versioned library package
# is named; zlib1g is in the base image but is listed explicitly since the
# server links against it
RUN apt-get update \
    && apt-get install -y --no-install-recommends ca-certificates openssl zlib1g \
    && rm -rf /var/lib/apt/lists/*

COPY --from=build /usr/local/bin/telegram-bot-api /usr/local/bin/telegram-bot-api
COPY entrypoint.sh /usr/local/bin/entrypoint.sh

WORKDIR /var/lib/telegram-bot-api
EXPOSE 8081
ENTRYPOINT ["/usr/local/bin/entrypoint.sh"]
CMD ["--verbosity=2", \
    "--storage-max-time-from-last-access=300", \
    "--http-idle-timeout=3600", \
    "--temp-dir=/var/lib/telegram-bot-api/temp"]
