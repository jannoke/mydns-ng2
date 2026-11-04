# MyDNS server image (MySQL/MariaDB backend).
#   docker build -t mydns .
#   docker compose up -d          # with MariaDB and a demo zone, see QUICKSTART.docker
#
# One binary supports one database backend, chosen at build time; this image is MySQL/MariaDB.

FROM debian:13 AS build
RUN apt-get update -qq \
 && DEBIAN_FRONTEND=noninteractive apt-get install -y -qq --no-install-recommends \
      build-essential autoconf automake libtool gettext autopoint texinfo \
      default-libmysqlclient-dev pkg-config ca-certificates \
 && rm -rf /var/lib/apt/lists/*
COPY . /src
WORKDIR /src
RUN ./bootstrap.sh \
 && ./configure --prefix=/usr --sysconfdir=/etc --with-mysql --without-pgsql \
 && make -j"$(nproc)" \
 && make install DESTDIR=/out \
 && rm -f /out/etc/mydns.conf

FROM debian:13-slim
RUN apt-get update -qq \
 && DEBIAN_FRONTEND=noninteractive apt-get install -y -qq --no-install-recommends \
      libmariadb3 mariadb-client bind9-dnsutils tini \
 && rm -rf /var/lib/apt/lists/*
COPY --from=build /out/usr/ /usr/
COPY docker/entrypoint.sh /usr/local/bin/docker-entrypoint.sh
RUN mkdir -p /docker-entrypoint.d

# Name the healthcheck asks for (its SOA). Set it to one of your zones.
ENV MYDNS_HEALTH_NAME=example.test
EXPOSE 53/udp 53/tcp
HEALTHCHECK --interval=30s --timeout=5s --start-period=30s --retries=3 \
  CMD dig +short +time=2 +tries=1 @127.0.0.1 "$MYDNS_HEALTH_NAME" SOA | grep -q . || exit 1

ENTRYPOINT ["tini", "--", "/usr/local/bin/docker-entrypoint.sh"]
CMD ["mydns"]
