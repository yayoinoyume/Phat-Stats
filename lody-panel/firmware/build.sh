#!/usr/bin/env bash
# Lody Panel 固件构建脚本。
#
#   ./build.sh             # 只编译
#   ./build.sh upload      # 编译后烧录到 PORT（默认 /dev/ttyUSB0）
#
# 可用环境变量覆盖：BUILD_DIR、FQBN、PORT、ARDUINO_CLI。
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
sketch_dir="$script_dir/LodyPanel"
build_dir="${BUILD_DIR:-/tmp/lody-panel-build}"
fqbn="${FQBN:-esp32:esp32:esp32:PartitionScheme=huge_app}"
port="${PORT:-/dev/ttyUSB0}"
arduino_cli="${ARDUINO_CLI:-arduino-cli}"

case "${1:-compile}" in
  compile) upload=0 ;;
  upload) upload=1 ;;
  *)
    echo "usage: $0 [compile|upload]" >&2
    exit 2
    ;;
esac

"$arduino_cli" compile \
  --fqbn "$fqbn" \
  --build-path "$build_dir" \
  --warnings all \
  "$sketch_dir"

if [[ "$upload" -eq 1 ]]; then
  "$arduino_cli" upload \
    --fqbn "$fqbn" \
    --port "$port" \
    --input-dir "$build_dir" \
    "$sketch_dir"
fi
