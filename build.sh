#!/usr/bin/env bash
# Developer build and tests in Docker (any machine with Docker, e.g. a Mac).
#   ./build.sh             unit tests (ASan+UBSan native, UBSan x86_64), then the
#                          static x86_64 release binary dist/blip
#   ./build.sh test [ID]   ASan+UBSan unit tests only (optional name filter)
#   ./build.sh image       (re)build both build images
# AddressSanitizer cannot run under x86 emulation, so the sanitizer tests use a native
# copy of the same image; the release is linux/amd64. Set DOCKER_CONTEXT to choose the
# Docker engine. To install on a server, use ./install.sh there instead.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
IMAGE="blip-build:ubuntu-26.04"
TEST_IMAGE="blip-build:ubuntu-26.04-test"

build_images() {
  docker build --target base --platform linux/amd64 -t "$IMAGE" -f "$ROOT/build/Dockerfile" "$ROOT"
  docker build --target base --platform "linux/$(docker version -f '{{.Server.Arch}}')" -t "$TEST_IMAGE" \
    -f "$ROOT/build/Dockerfile" "$ROOT"
}

ensure_images() {
  docker image inspect "$IMAGE" >/dev/null 2>&1 &&
    docker image inspect "$TEST_IMAGE" >/dev/null 2>&1 || build_images
}

run_make() { # <image> make args...
  local image="$1"; shift
  local plat=()
  [ "$image" = "$IMAGE" ] && plat=(--platform linux/amd64)
  docker run --rm ${plat[@]+"${plat[@]}"} -u "$(id -u):$(id -g)" \
    -e HOME=/tmp -v "$ROOT:/src" -w /src "$image" make "$@"
}

case "${1:-build}" in
  image) build_images ;;
  test) ensure_images; rm -f "$ROOT/.build/run"; run_make "$TEST_IMAGE" test FILTER="${2:-}" ;;
  build)
    ensure_images
    rm -f "$ROOT/.build/run" "$ROOT/.build/run-ubsan"
    run_make "$TEST_IMAGE" test
    run_make "$IMAGE" test-ubsan
    run_make "$IMAGE" dist/blip
    ls -l "$ROOT/dist/blip" ;;
  *) sed -n '2,9p' "$0" >&2; exit 2 ;;
esac
