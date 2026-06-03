#!/usr/bin/env bash
#
# Configure and build the CMF back-tester via CMake.
#
# Usage:
#   scripts/build.sh [options]
#
# Options:
#   -t, --type <Debug|Release>   Build type (default: Release)
#   -B, --build-dir <dir>        Build directory (default: build)
#   -j, --jobs <N>               Parallel build jobs (default: all cores)
#   -c, --clean                  Remove the build directory before configuring
#       --no-tests               Configure with -DBUILD_TESTS=OFF
#       --no-benchmarks          Configure with -DBUILD_BENCHMARKS=OFF
#   -T, --test                   Run ctest after a successful build
#   -h, --help                   Show this help and exit
#
# Examples:
#   scripts/build.sh                      # Release build with tests + benchmarks
#   scripts/build.sh -t Debug -T          # Debug build, then run the test suite
#   scripts/build.sh -c --no-benchmarks   # Clean reconfigure without benchmarks

set -euo pipefail

# Resolve the repository root (this script lives in <root>/scripts).
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"

BUILD_TYPE="Release"
BUILD_DIR="build"
CLEAN=0
RUN_TESTS=0
BUILD_TESTS="ON"
BUILD_BENCHMARKS="ON"

# Default to the number of available cores.
if command -v nproc >/dev/null 2>&1; then
    JOBS="$(nproc)"
elif command -v sysctl >/dev/null 2>&1; then
    JOBS="$(sysctl -n hw.ncpu)"
else
    JOBS=4
fi

usage() {
    # Print the leading comment block (everything after the shebang up to the
    # first non-comment line), stripping the leading "# ".
    awk 'NR==1 && /^#!/ {next} /^#/ {sub(/^# ?/, ""); print; next} {exit}' \
        "${BASH_SOURCE[0]}"
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        -t|--type)
            BUILD_TYPE="${2:?missing value for $1}"; shift 2 ;;
        -B|--build-dir)
            BUILD_DIR="${2:?missing value for $1}"; shift 2 ;;
        -j|--jobs)
            JOBS="${2:?missing value for $1}"; shift 2 ;;
        -c|--clean)
            CLEAN=1; shift ;;
        --no-tests)
            BUILD_TESTS="OFF"; shift ;;
        --no-benchmarks)
            BUILD_BENCHMARKS="OFF"; shift ;;
        -T|--test)
            RUN_TESTS=1; shift ;;
        -h|--help)
            usage; exit 0 ;;
        *)
            echo "Unknown option: $1" >&2
            usage >&2
            exit 1 ;;
    esac
done

cd "${ROOT_DIR}"

if [[ "${CLEAN}" -eq 1 ]]; then
    echo ">>> Removing build directory: ${BUILD_DIR}"
    rm -rf "${BUILD_DIR}"
fi

echo ">>> Configuring (${BUILD_TYPE}) in '${BUILD_DIR}' [tests=${BUILD_TESTS} benchmarks=${BUILD_BENCHMARKS}]"
cmake -B "${BUILD_DIR}" -S . \
    -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" \
    -DBUILD_TESTS="${BUILD_TESTS}" \
    -DBUILD_BENCHMARKS="${BUILD_BENCHMARKS}"

echo ">>> Building with ${JOBS} job(s)"
cmake --build "${BUILD_DIR}" -j "${JOBS}"

if [[ "${RUN_TESTS}" -eq 1 ]]; then
    if [[ "${BUILD_TESTS}" == "ON" ]]; then
        echo ">>> Running tests"
        ctest --test-dir "${BUILD_DIR}" -j "${JOBS}" --output-on-failure
    else
        echo ">>> Skipping tests (configured with --no-tests)" >&2
    fi
fi

echo ">>> Done. Artifacts in ${BUILD_DIR}/bin"
