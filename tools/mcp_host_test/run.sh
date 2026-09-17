#!/usr/bin/env sh
# Compile components/eye_web/eye_mcp.c on the host against fake motion/servo
# calls and run the protocol tests. No board needed.
#
# cJSON comes from managed_components/, which appears after the first
# `pio run`. Usage: tools/mcp_host_test/run.sh
set -eu
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
cjson="$root/managed_components/espressif__cjson/cJSON"
[ -f "$cjson/cJSON.c" ] || { echo "cJSON not found: run 'pio run' once first" >&2; exit 1; }
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT
cc -std=c11 -D_DEFAULT_SOURCE -Wall -Wextra -Werror -Wno-unused-parameter -g \
   -fsanitize=address,undefined \
   -I"$here/stub" -I"$root/components/eye_web" \
   -I"$root/components/eye_motion/include" -I"$root/components/eye_servo/include" \
   -I"$root/components/eye_vision/include" -I"$root/components/pca9685/include" -I"$cjson" \
   "$here/test_mcp.c" "$here/fakes.c" "$root/components/eye_web/eye_mcp.c" "$cjson/cJSON.c" \
   -lm -o "$out/test_mcp"
"$out/test_mcp"
