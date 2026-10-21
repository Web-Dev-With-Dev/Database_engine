#!/usr/bin/env bash
# ==============================================================================
# run_helgrind.sh - Run Concurrent Tests under Valgrind Helgrind
#
# Helgrind is a POSIX thread error detector that detects:
# 1. Misuses of the POSIX pthreads API
# 2. Lock-order problems (potential deadlocks)
# 3. Data races (concurrent access without synchronization)
# ==============================================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
BUILD_DIR="${ROOT_DIR}/build-helgrind"

if ! command -v valgrind &> /dev/null; then
    echo "ERROR: valgrind is not installed. Install via 'sudo apt-get install valgrind' or package manager."
    exit 1
fi

echo "=========================================================="
echo " Building DittoDB Debug Binary for Helgrind"
echo "=========================================================="

cmake -B "${BUILD_DIR}" -S "${ROOT_DIR}" -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug

cmake --build "${BUILD_DIR}" --target buffer_pool_manager_concurrent_test

echo ""
echo "=========================================================="
echo " Running BufferPoolManager Concurrent Tests under Helgrind"
echo "=========================================================="

valgrind \
  --tool=helgrind \
  --error-exitcode=1 \
  --read-var-info=yes \
  --track-lockorders=yes \
  "${BUILD_DIR}/buffer_pool_manager_concurrent_test"

echo ""
echo ">>> SUCCESS: Helgrind completed with 0 errors, 0 lock cycles, and 0 data races! <<<"
