#!/bin/sh
# Builds macapad.vpk with VitaSDK's Docker image (no local VitaSDK install needed).
# ./build.sh          clean build into build/, then copies macapad.vpk here
# ./build.sh clean    removes build output
set -e
cd "$(dirname "$0")"
if [ "$1" = "clean" ]; then
	rm -rf build macapad.vpk
	exit 0
fi
# the image is x86_64 only; Docker Desktop runs it under emulation on Apple Silicon
docker run --rm --platform linux/amd64 -v "$PWD":/src -w /src vitasdk/vitasdk:latest sh -c '
	rm -rf build &&
	cmake -S . -B build -DVITA_VERSION="${VITA_VERSION:-01.00}" >/dev/null &&
	cmake --build build -j"$(nproc)"
'
cp build/macapad.vpk .
echo "built macapad.vpk"
