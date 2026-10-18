#!/bin/sh
# Compiles m5stick_glove_kb inside Docker, so there is no ESP32 toolchain to install.
#
#   ./docker-build.sh                        M5StickC Plus, the board this runs on
#   ./docker-build.sh m5stack_stickc_plus2   compiles for the PLUS2; never tried on one
#
# Output lands in build/. Flash from the host with esptool (see README).
set -e

BOARD="${1:-m5stack_stickc_plus}"
case "$BOARD" in ''|*[!a-z0-9_]*) echo "bad board name: $BOARD" >&2; exit 2 ;; esac

# The toolchain and libraries are pinned to what the working firmware was built with.
IMAGE=debian:13-slim@sha256:a29215f6a35e51e22adffa17f89e9d2ef06214e64a2bad10d765c46aea49f11f
ARDUINO_CLI=1.5.1
ARDUINO_CLI_SHA_X86=28a8e119c498a25607821c36cb2dc49e8463941b261a0d99091baa7bc692dd2b
ARDUINO_CLI_SHA_ARM=1e69e077479f300614d4551334e0a33f08ee40b04315d83b8e7e0e94f0d0ee62
ESP32_CORE=3.3.10
ESP32_INDEX_SHA=612accfa65b9bd27c460b5c5a150b9bbdb431bbe
NIMBLE=2.5.0
M5GFX=0.2.25
M5UNIFIED=0.2.18
KB_LIB=https://github.com/Berg0162/ESP32-NIMBLE-Keyboard.git
KB_LIB_SHA=1f04c0496a398924bf321d7b27956beab485deb2

# The container can read this script and the sketch but not change them. build/ is all it can write.
if [ ! -f /.dockerenv ]; then
  HERE="$(cd "$(dirname "$0")" && pwd)"
  mkdir -p "$HERE/build"
  exec docker run --rm \
    -v "$HERE/docker-build.sh:/src/docker-build.sh:ro" \
    -v "$HERE/m5stick_glove_kb:/src/m5stick_glove_kb:ro" \
    -v "$HERE/build:/out" \
    -v pg-arduino-cache:/root/.arduino15 \
    "$IMAGE" sh /src/docker-build.sh "$BOARD"
fi

apt-get update -qq && apt-get install -y -qq curl git ca-certificates python3 >/dev/null

case "$(uname -m)" in
  x86_64)  CLI_ARCH=64bit; CLI_SHA=$ARDUINO_CLI_SHA_X86 ;;
  aarch64) CLI_ARCH=ARM64; CLI_SHA=$ARDUINO_CLI_SHA_ARM ;;
  *) echo "no pinned arduino-cli for $(uname -m)" >&2; exit 2 ;;
esac
curl -fsSL -o /tmp/arduino-cli.tar.gz "https://github.com/arduino/arduino-cli/releases/download/v$ARDUINO_CLI/arduino-cli_${ARDUINO_CLI}_Linux_$CLI_ARCH.tar.gz"
echo "$CLI_SHA  /tmp/arduino-cli.tar.gz" | sha256sum -c - >/dev/null
tar -xzf /tmp/arduino-cli.tar.gz -C /usr/local/bin arduino-cli
arduino-cli config init --overwrite >/dev/null
arduino-cli config set board_manager.additional_urls "https://raw.githubusercontent.com/espressif/arduino-esp32/$ESP32_INDEX_SHA/package_esp32_index.json"
arduino-cli core update-index >/dev/null
echo "[*] installing esp32 core $ESP32_CORE (cached in the pg-arduino-cache volume after the first run)..."
arduino-cli core install "esp32:esp32@$ESP32_CORE"
arduino-cli lib install --no-deps "NimBLE-Arduino@$NIMBLE" "M5GFX@$M5GFX" "M5Unified@$M5UNIFIED" >/dev/null
KB_DIR=/root/Arduino/libraries/ESP32-NIMBLE-Keyboard
if [ ! -d "$KB_DIR" ]; then
  git init -q "$KB_DIR"
  git -C "$KB_DIR" fetch -q --depth 1 "$KB_LIB" "$KB_LIB_SHA"
  git -C "$KB_DIR" checkout -q FETCH_HEAD
fi

echo "[*] compiling m5stick_glove_kb for $BOARD ..."
mkdir -p /work/m5stick_glove_kb                       # the toolchain writes beside the sketch, so compile a copy
cp /src/m5stick_glove_kb/*.ino /work/m5stick_glove_kb/
arduino-cli compile --fqbn "esp32:esp32:$BOARD" --export-binaries /work/m5stick_glove_kb
cp "/work/m5stick_glove_kb/build/esp32.esp32.$BOARD"/*.bin /out/
ls -la /out/
echo "[done]"
