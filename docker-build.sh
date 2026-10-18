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
ARDUINO_CLI=1.5.1
ESP32_CORE=3.3.10
NIMBLE=2.5.0
M5GFX=0.2.25
M5UNIFIED=0.2.18
KB_LIB=https://github.com/Berg0162/ESP32-NIMBLE-Keyboard.git
KB_LIB_SHA=1f04c0496a398924bf321d7b27956beab485deb2

if [ ! -f /.dockerenv ]; then
  exec docker run --rm -v "$(pwd):/work" -v pg-arduino-cache:/root/.arduino15 -w /work debian:13-slim sh /work/docker-build.sh "$BOARD"
fi

apt-get update -qq && apt-get install -y -qq curl git ca-certificates python3 >/dev/null

command -v arduino-cli >/dev/null 2>&1 || \
  curl -fsSL "https://raw.githubusercontent.com/arduino/arduino-cli/v$ARDUINO_CLI/install.sh" | BINDIR=/usr/local/bin sh -s "$ARDUINO_CLI" >/dev/null
arduino-cli config init --overwrite >/dev/null
arduino-cli config set board_manager.additional_urls https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json
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
arduino-cli compile --fqbn "esp32:esp32:$BOARD" --export-binaries /work/m5stick_glove_kb
mkdir -p /work/build
cp "/work/m5stick_glove_kb/build/esp32.esp32.$BOARD"/*.bin /work/build/
ls -la /work/build/
echo "[done]"
