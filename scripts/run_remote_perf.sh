#!/usr/bin/env bash
# Run PagedLOB hardware PMU counter & real NASDAQ ITCH 5.0 replay benchmarks over SSH
# Streams current git HEAD via `git archive` (no GitHub auth needed on the remote host).
#
# Usage from your local machine:
#   ./scripts/run_remote_perf.sh remote
#   ./scripts/run_remote_perf.sh quant-wsl
#   ./scripts/run_remote_perf.sh remote --download-itch
#   ./scripts/run_remote_perf.sh remote /path/on/remote/12302019.NASDAQ_ITCH50.gz

set -euo pipefail

HOST="${1:-remote}"
ITCH_ARG="${2:-}"
REPO_ROOT="$(git rev-parse --show-toplevel)"

echo ">>> Syncing HEAD of PagedLOB to '${HOST}:~/.cache/pagedlob-bench' via git archive..."
git -C "${REPO_ROOT}" archive --format=tar HEAD | ssh "${HOST}" '
  set -euo pipefail
  mkdir -p "$HOME/.cache/pagedlob-bench/src"
  rm -rf "$HOME/.cache/pagedlob-bench/src"/*
  tar -xf - -C "$HOME/.cache/pagedlob-bench/src"
'

echo ">>> Building Release (-O3 -DNDEBUG) & collecting Linux PMU counters on '${HOST}'..."
ssh "${HOST}" bash -s -- "${ITCH_ARG}" << 'EOF'
set -euo pipefail
ITCH_ARG="${1:-}"
WORK_DIR="$HOME/.cache/pagedlob-bench"
cd "$WORK_DIR/src"

CXX_BIN="${CXX:-g++}"
if command -v g++-13 >/dev/null 2>&1; then CXX_BIN="g++-13"; fi

cmake -S . -B "$WORK_DIR/build-perf" \
  -DCMAKE_CXX_COMPILER="$CXX_BIN" \
  -DCMAKE_BUILD_TYPE=Release \
  -DFASTLOB_ENABLE_ALLOC_PROBE=OFF \
  -DFASTLOB_BUILD_TESTS=OFF \
  -DFASTLOB_BUILD_PYTHON=OFF

cmake --build "$WORK_DIR/build-perf" -j"$(nproc)" \
  --target price_index_bench external_compare_bench itch_replay_bench

PIN=()
if command -v taskset >/dev/null 2>&1; then
  PIN=(taskset -c 0)
fi

echo "=== Host CPU & Kernel PMU Status ==="
grep -m1 "model name" /proc/cpuinfo || true
echo -n "perf_event_paranoid: "
cat /proc/sys/kernel/perf_event_paranoid 2>/dev/null || echo "unknown"

echo "=== [1/3] price_index_bench (emits BEGIN_PERF_MD when PMU counters are active) ==="
"${PIN[@]}" "$WORK_DIR/build-perf/price_index_bench"

echo "=== [2/3] external_compare_bench (PagedLOB vs Liquibook vs AVL-tree book) ==="
"${PIN[@]}" "$WORK_DIR/build-perf/external_compare_bench"

ITCH_FILE=""
if [ "$ITCH_ARG" = "--download-itch" ]; then
  mkdir -p "$WORK_DIR/itch_data"
  ITCH_FILE="$WORK_DIR/itch_data/12302019.NASDAQ_ITCH50.gz"
  if [ ! -s "$ITCH_FILE" ]; then
    echo ">>> Downloading official 12302019.NASDAQ_ITCH50.gz to $ITCH_FILE ..."
    curl -L --retry 5 -C - -o "$ITCH_FILE" \
      "https://emi.nasdaq.com/ITCH/Nasdaq%20ITCH/12302019.NASDAQ_ITCH50.gz"
  fi
elif [ -n "$ITCH_ARG" ] && [ -f "$ITCH_ARG" ]; then
  ITCH_FILE="$ITCH_ARG"
fi

if [ -n "$ITCH_FILE" ]; then
  echo "=== [3/3] itch_replay_bench on $ITCH_FILE (AAPL, SPY, MSFT) ==="
  mkdir -p "$WORK_DIR/itch_data"
  if [ -f "$WORK_DIR/itch_data/AAPL.bin" ] && [ -f "$WORK_DIR/itch_data/SPY.bin" ] && [ -f "$WORK_DIR/itch_data/MSFT.bin" ]; then
    "${PIN[@]}" "$WORK_DIR/build-perf/itch_replay_bench" "$ITCH_FILE" \
      --bin-dir="$WORK_DIR/itch_data" --symbols=AAPL,SPY,MSFT --reps=5 --batch=128
  else
    "${PIN[@]}" "$WORK_DIR/build-perf/itch_replay_bench" "$ITCH_FILE" \
      --save-dir="$WORK_DIR/itch_data" --symbols=AAPL,SPY,MSFT --reps=5 --batch=128
  fi
fi
EOF
