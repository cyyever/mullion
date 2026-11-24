#!/bin/sh
set -eu

UNIX_INSTALL_DIR=$2
INSTALL_DIR="${DESTDIR%%/}${MESON_INSTALL_PREFIX}/${UNIX_INSTALL_DIR}"

mkdir -p "${INSTALL_DIR}"
cp "${MESON_BUILD_ROOT}/src/winemetal/unix/winemetal.so" "${INSTALL_DIR}/winemetal.so"

if [ "$1" = "true" ]; then
    strip -x "${INSTALL_DIR}/winemetal.so"
fi

codesign --force --sign - --timestamp=none "${INSTALL_DIR}/winemetal.so"
