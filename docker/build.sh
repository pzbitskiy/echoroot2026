#!/bin/sh
# Run a command (default: make) inside the echoroot build container.
#   docker/build.sh                 # full build -> sdcard/images/sdcard.img
#   docker/build.sh make -C rootfs  # rootfs only
#   docker/build.sh bash            # interactive shell (needs a terminal)
#
# The image is built from docker/Dockerfile only if it does not exist yet, so
# an image restored with "docker load" is used as it is and no Ubuntu package
# server is needed. Set REBUILD_IMAGE=1 to rebuild it after editing the
# Dockerfile.
set -e
cd "$(dirname "$0")/.."

if [ -n "$REBUILD_IMAGE" ] || ! docker image inspect echoroot-build >/dev/null 2>&1; then
    docker build -q -t echoroot-build \
        --build-arg UID="$(id -u)" --build-arg GID="$(id -g)" docker >/dev/null
fi

[ $# -eq 0 ] && set -- make
TTY=; [ -t 0 ] && TTY=-t
exec docker run --rm -i $TTY \
    -v "$PWD":/sata/src/echoroot \
    echoroot-build "$@"
