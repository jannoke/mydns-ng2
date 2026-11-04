#!/bin/sh
# Build the Debian 13 (trixie) package in a container.
#   pkg/build-deb.sh [output-dir]      (default: ./dist)
# The source tree is copied into the container (without .git and build
# output), so an in-tree build does not get in the way.
set -eu
top=$(cd "$(dirname "$0")/.." && pwd)
out=$(mkdir -p "${1:-$top/dist}" && cd "${1:-$top/dist}" && pwd)
image=${DEBIAN_IMAGE:-debian:13}
docker=${DOCKER:-docker}

"$docker" run --rm -i \
	-v "$top:/src:ro" -v "$out:/out" \
	-e HOST_UID="$(id -u)" -e HOST_GID="$(id -g)" \
	"$image" sh -eu <<'SCRIPT'
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y -qq --no-install-recommends devscripts equivs rsync lintian >/dev/null
mkdir -p /build/mydns
rsync -a --exclude=/.git --exclude=/dist --exclude=/.env --exclude='*.log' \
	--exclude='*.o' --exclude='*.a' --exclude=.deps --exclude=autom4te.cache \
	--exclude=Makefile --exclude=Makefile.in --exclude=/aclocal.m4 --exclude=/configure \
	--exclude=/config.h --exclude=/config.status --exclude=/libtool --exclude=/stamp-h1 \
	/src/ /build/mydns/
cd /build/mydns
mk-build-deps -i -r -t 'apt-get -y -qq --no-install-recommends' debian/control >/dev/null
rm -f mydns-build-deps_*
dpkg-buildpackage -us -uc -b
cd /build
lintian *.changes || true
cp -v *.deb *.buildinfo *.changes /out/
chown "$HOST_UID:$HOST_GID" /out/*
SCRIPT
