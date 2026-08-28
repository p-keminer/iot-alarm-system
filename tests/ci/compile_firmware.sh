#!/usr/bin/env bash
set -euo pipefail

readonly REPO_ROOT="$(git rev-parse --show-toplevel)"
readonly ESP_FQBN="esp8266:esp8266:nodemcuv2"
readonly UNO_FQBN="arduino:avr:uno"
readonly SENDER_DIR="$REPO_ROOT/firmware/esp8266/sender"
readonly RECEIVER_DIR="$REPO_ROOT/firmware/esp8266/receiver"
readonly UNO_DIR="$REPO_ROOT/firmware/elegoo_uno_r3/sketchR3/alarm_system"
readonly TEMP_ROOT="$(mktemp -d "${RUNNER_TEMP:-${TMPDIR:-/tmp}}/iot-alarm-firmware.XXXXXX")"

cleanup() {
  rm -rf -- "$TEMP_ROOT"
}
trap cleanup EXIT

require_primary_sketch() {
  local sketch_dir="$1"
  local primary_name="$2"

  if [[ ! -f "$sketch_dir/$primary_name" ]]; then
    printf 'Required primary sketch is missing: %s\n' "$sketch_dir/$primary_name" >&2
    exit 1
  fi
}

compile_esp() {
  local component="$1"
  local sketch_dir="$2"
  local telnet_mode="$3"
  local variant="${component}-telnet-${telnet_mode}"

  printf '\nCompiling %s (ALARM_ENABLE_TELNET=%s)\n' "$component" "$telnet_mode"
  arduino-cli compile \
    --fqbn "$ESP_FQBN" \
    --warnings all \
    --build-property "compiler.cpp.extra_flags=-DALARM_ENABLE_TELNET=${telnet_mode}" \
    --build-path "$TEMP_ROOT/build-$variant" \
    "$sketch_dir"
}

copy_uno_sketch() {
  local destination="$1"

  mkdir -p "$destination/alarm_system"
  cp -a "$UNO_DIR/." "$destination/alarm_system/"
  rm -f -- "$destination/alarm_system/uid_whitelist.local.h"
}

require_primary_sketch "$SENDER_DIR" "sender.ino"
require_primary_sketch "$RECEIVER_DIR" "receiver.ino"
require_primary_sketch "$UNO_DIR" "alarm_system.ino"

if git -C "$REPO_ROOT" ls-files --error-unmatch \
  firmware/elegoo_uno_r3/sketchR3/alarm_system/uid_whitelist.local.h \
  >/dev/null 2>&1; then
  printf 'Private RFID whitelist must never be tracked.\n' >&2
  exit 1
fi

compile_esp sender "$SENDER_DIR" 0
compile_esp sender "$SENDER_DIR" 1
compile_esp receiver "$RECEIVER_DIR" 0
compile_esp receiver "$RECEIVER_DIR" 1

copy_uno_sketch "$TEMP_ROOT/uno-default"
printf '\nCompiling Uno without a local UID whitelist (fail-closed default)\n'
arduino-cli compile \
  --fqbn "$UNO_FQBN" \
  --warnings all \
  --build-path "$TEMP_ROOT/build-uno-default" \
  "$TEMP_ROOT/uno-default/alarm_system"

copy_uno_sketch "$TEMP_ROOT/uno-example"
printf '%s\n' \
  '#ifndef UID_WHITELIST_LOCAL_H' \
  '#define UID_WHITELIST_LOCAL_H' \
  '#define ALARM_UID_WHITELIST_ENTRIES { { { 0xDE, 0xAD, 0xBE, 0xEF }, 4 } }' \
  '#endif' \
  > "$TEMP_ROOT/uno-example/alarm_system/uid_whitelist.local.h"

printf '\nCompiling Uno with a generated, non-secret example UID\n'
arduino-cli compile \
  --fqbn "$UNO_FQBN" \
  --warnings all \
  --build-path "$TEMP_ROOT/build-uno-example" \
  "$TEMP_ROOT/uno-example/alarm_system"
