# skip building in internal infra:
# DOCKER_SKIP

FROM debian:trixie-slim AS build

RUN apt-get update \
  && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
  build-essential \
  ca-certificates \
  curl \
  default-libmysqlclient-dev \
  g++ \
  gcc \
  git \
  gperf \
  libavcodec-extra \
  libavfilter-dev \
  libbcg729-dev \
  libcurl4-openssl-dev \
  libevent-dev \
  libgrpc++-dev \
  libglib2.0-dev \
  libhiredis-dev \
  libiptc-dev \
  libjson-glib-dev \
  libjwt-dev \
  libncursesw5-dev \
  libopus-dev \
  libpcap-dev \
  libpcre2-dev \
  libprotobuf-dev \
  libspandsp-dev \
  libssl-dev \
  libwebsockets-dev \
  make \
  markdown \
  patch \
  protobuf-compiler \
  protobuf-compiler-grpc

WORKDIR /usr/src/rtpengine
COPY . .

FROM build AS rtpengine
WORKDIR /usr/src/rtpengine/daemon
RUN make -j$(nproc) rtpengine && \
  strip -o /usr/local/bin/rtpengine rtpengine

FROM build AS rtpengine-recording
WORKDIR /usr/src/rtpengine/recording-daemon
RUN make -j$(nproc) rtpengine-recording with_grpc=yes && \
  strip -o /usr/local/bin/rtpengine-recording rtpengine-recording

FROM debian:trixie-slim

VOLUME ["/rec"]
ENTRYPOINT ["/entrypoint.sh"]
CMD ["rtpengine"]

EXPOSE 23000-65535/udp 22222/udp

RUN apt-get update \
  && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
  curl \
  libavcodec-extra \
  libavfilter10 \
  libbcg729-0 \
  libcurl4 \
  libevent-2.1-7 \
  libevent-pthreads-2.1-7 \
  libgrpc++1.51t64 \
  libglib2.0-0 \
  libhiredis1.1.0 \
  libip4tc2 \
  libip6tc2 \
  libjson-glib-1.0-0 \
  libjwt2 \
  libmariadb3 \
  libncursesw6 \
  libopus0 \
  libpcap0.8 \
  libpcre2-8-0 \
  libprotobuf32t64 \
  libspandsp2 \
  libssl3 \
  libwebsockets19 \
  iproute2 \
  kmod \
  net-tools \
  procps \
  && apt-get clean && rm -rf /var/lib/apt/lists/*

COPY --from=rtpengine /usr/local/bin/rtpengine /usr/local/bin/
COPY --from=rtpengine-recording /usr/local/bin/rtpengine-recording /usr/local/bin/
COPY docker/entrypoint.sh /entrypoint.sh
USER root
WORKDIR /root
COPY docker/rtpengine.conf .
