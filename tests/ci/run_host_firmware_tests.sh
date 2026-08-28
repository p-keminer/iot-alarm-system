#!/usr/bin/env bash
set -euo pipefail

readonly REPO_ROOT="$(git rev-parse --show-toplevel)"
readonly TEST_OUTPUT="${RUNNER_TEMP:-/tmp}/iot-alarm-host-tests"
mkdir -p "$TEST_OUTPUT"
cd "$REPO_ROOT"

g++ -std=c++17 -Wall -Wextra -Werror \
  -Itests/runtime_deploy/stubs \
  -Ifirmware/elegoo_uno_r3/sketchR3/alarm_system \
  tests/runtime_deploy/uno_fsm_host_test.cpp \
  -o "$TEST_OUTPUT/uno_fsm_host_test"
"$TEST_OUTPUT/uno_fsm_host_test"

g++ -std=c++17 -Wall -Wextra -Werror \
  tests/security_protocol/replay_schutz_host_test.cpp \
  -o "$TEST_OUTPUT/replay_schutz_host_test"
"$TEST_OUTPUT/replay_schutz_host_test"

printf 'Host firmware safety tests: ok\n'
