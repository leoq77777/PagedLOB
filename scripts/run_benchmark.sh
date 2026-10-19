#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="${BUILD_DIR:-${ROOT}/build-release}"
JOBS="${JOBS:-$(sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4)}"
BURST="${BURST:-1000000}"
BATCH="${BATCH:-64}"
RAW_LOG="${BUILD}/bench-latest.txt"
mkdir -p "${BUILD}"

{
  echo "=== FastLOB benchmark harness ==="
  echo "date:        $(date -u +%Y-%m-%dT%H:%M:%SZ)"
  echo "uname:       $(uname -a)"
  if [[ "$(uname -s)" == "Darwin" ]]; then
    echo "cpu:         $(sysctl -n machdep.cpu.brand_string 2>/dev/null || echo unknown)"
    echo "ncpu:        $(sysctl -n hw.ncpu 2>/dev/null || echo unknown)"
  else
    echo "cpu:         $(grep -m1 'model name' /proc/cpuinfo 2>/dev/null | cut -d: -f2 | sed 's/^ //' || echo unknown)"
    echo "ncpu:        $(nproc 2>/dev/null || echo unknown)"
  fi
  echo "compiler:    $(c++ --version | head -1)"
  echo "cmake:       $(cmake --version | head -1)"
  echo "cxx_flags:   -O3 -DNDEBUG -std=c++20 (Release)"
  echo "workload:    synthetic MBO 20k events seed=123; cancel+re-add burst=${BURST} batch=${BATCH} (1k warm ids)"
  echo "build_dir:   ${BUILD}"
  echo
} | tee "${RAW_LOG}"

cmake -S "${ROOT}" -B "${BUILD}" \
  -DCMAKE_BUILD_TYPE=Release \
  -DFASTLOB_BUILD_TESTS=ON \
  -DFASTLOB_BUILD_BENCH=ON \
  -DFASTLOB_BUILD_PYTHON=ON \
  -DFASTLOB_ENABLE_ALLOC_PROBE=ON

cmake --build "${BUILD}" -j "${JOBS}" --target fastlob_tests fastlob_bench fastlob_latency_hist

echo | tee -a "${RAW_LOG}"
echo "=== Unit / differential / zero-alloc tests ===" | tee -a "${RAW_LOG}"
ctest --test-dir "${BUILD}" --output-on-failure | tee -a "${RAW_LOG}"

echo | tee -a "${RAW_LOG}"
echo "=== Google Benchmark (aggregates) ===" | tee -a "${RAW_LOG}"
"${BUILD}/fastlob_bench" \
  --benchmark_filter='BM_Fast_CancelReadd|BM_Fast_ApplyStream|BM_Reference_ApplyStream' \
  --benchmark_repetitions=5 \
  --benchmark_report_aggregates_only=true \
  --benchmark_display_aggregates_only=true \
  --benchmark_min_time=0.2s 2>&1 | tee -a "${RAW_LOG}"

echo | tee -a "${RAW_LOG}"
echo "=== Latency histogram (p50/p99/p999) ===" | tee -a "${RAW_LOG}"
"${BUILD}/fastlob_latency_hist" --burst "${BURST}" --batch "${BATCH}" 2>&1 | tee -a "${RAW_LOG}"

echo | tee -a "${RAW_LOG}"
echo "Raw log: ${RAW_LOG}"
echo "Done."
