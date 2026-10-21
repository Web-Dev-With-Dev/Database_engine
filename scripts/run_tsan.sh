#!/usr/bin/env bash
# ==============================================================================
# run_tsan.sh - Build and Run Test Suite with ThreadSanitizer (TSan)
#
# ThreadSanitizer detects data races, lock order inversions, and thread leaks
# at runtime. Supported on Linux (x86_64/arm64) and macOS with Clang or GCC.
# ==============================================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
BUILD_DIR="${ROOT_DIR}/build-tsan"

echo "=========================================================="
echo " Building DittoDB with ThreadSanitizer (TSan)"
echo "=========================================================="

cmake -B "${BUILD_DIR}" -S "${ROOT_DIR}" -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DENABLE_TSAN=ON

cmake --build "${BUILD_DIR}"

echo ""
echo "=========================================================="
echo " Running Concurrent Stress Tests under ThreadSanitizer"
echo "=========================================================="

export TSAN_OPTIONS="halt_on_error=1 abort_on_error=1 second_deadlock_stack=1"

ctest --test-dir "${BUILD_DIR}" -R buffer_pool_manager_concurrent_test --output-on-failure --verbose

echo ""
echo "=========================================================="
echo " Running All 191 Tests under ThreadSanitizer"
echo "=========================================================="

ctest --test-dir "${BUILD_DIR}" --output-on-failure

echo ""
echo ">>> SUCCESS: All tests passed with 0 data races detected by ThreadSanitizer! <<<"
