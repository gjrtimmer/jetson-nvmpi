#!/usr/bin/env bash
# Build (and optionally install) the libnvmpi shared library.
#
# By default it builds libnvmpi against the real Jetson Multimedia API
# libraries; if those are not present (e.g. building off-Jetson or in CI) it
# automatically falls back to the stubs in stubs/ so the library still compiles
# (but is NOT runnable).
#
# Usage:
#   scripts/build.sh [options]
#
#   --stubs            Force linking against stubs/ (-DWITH_STUBS=ON).
#   --no-stubs         Force linking against real Jetson libraries.
#   --install          Install (uses sudo if not root) + ldconfig.
#   --clean            Remove the build directory before configuring.
#   --package          Build a .deb package (CPack) after building.
#   --build-dir DIR    Build directory (default: <repo>/build).
#   --prefix DIR       CMAKE_INSTALL_PREFIX (default: /usr/local).
#   --build-type TYPE  CMAKE_BUILD_TYPE (default: Release).
#   -j N               Parallel build jobs (default: nproc).
#   -h, --help         Show this help.
#
# Quick builds:
#   scripts/build.sh                 # libnvmpi only
#   scripts/build.sh --install       # libnvmpi, installed system-wide
#   scripts/build.sh --stubs         # libnvmpi against stubs (off-Jetson / CI)
#
# Safe to run from any working directory.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

# Defaults
BUILD_DIR="${REPO_ROOT}/build"
BUILD_TYPE="Release"
JOBS="$(nproc)"
DO_INSTALL=0
DO_CLEAN=0
DO_PACKAGE=0
PREFIX="/usr/local"   # CMake's default install prefix; override with --prefix
WITH_STUBS=""   # "", "ON" or "OFF"; "" means auto-detect
JETSON_API_DIR="${JETSON_MULTIMEDIA_API_DIR:-/usr/src/jetson_multimedia_api}"
# JP6 moved tegra libs from tegra/ to nvidia/; auto-detect
if [ -d "/usr/lib/aarch64-linux-gnu/nvidia" ]; then
    _DEFAULT_LIB_DIR="/usr/lib/aarch64-linux-gnu/nvidia"
else
    _DEFAULT_LIB_DIR="/usr/lib/aarch64-linux-gnu/tegra"
fi
JETSON_LIB_DIR="${JETSON_MULTIMEDIA_LIB_DIR:-${_DEFAULT_LIB_DIR}}"

usage() { sed -n '2,/^set /{/^set /d;s/^# \{0,1\}//;p}' "${BASH_SOURCE[0]}"; }

while [ $# -gt 0 ]; do
    case "$1" in
        --stubs)        WITH_STUBS="ON" ;;
        --no-stubs)     WITH_STUBS="OFF" ;;
        --install)      DO_INSTALL=1 ;;
        --package)      DO_PACKAGE=1 ;;
        --clean)        DO_CLEAN=1 ;;
        --build-dir)    BUILD_DIR="$2"; shift ;;
        --prefix)       PREFIX="$2"; shift ;;
        --build-type)   BUILD_TYPE="$2"; shift ;;
        -j)             JOBS="$2"; shift ;;
        -j*)            JOBS="${1#-j}" ;;
        -h|--help)      usage; exit 0 ;;
        *) echo "[E] unknown option: $1" >&2; usage; exit 1 ;;
    esac
    shift
done

# Auto-detect stubs vs real Jetson libraries when not forced.
if [ -z "${WITH_STUBS}" ]; then
    if [ -d "${JETSON_API_DIR}" ] && [ -d "${JETSON_LIB_DIR}" ]; then
        WITH_STUBS="OFF"
        echo "[i] Jetson Multimedia API detected -> building against real libraries."
    else
        WITH_STUBS="ON"
        echo "[i] Jetson Multimedia API not found -> building against stubs/ (not runnable)."
    fi
fi

if [ "${DO_CLEAN}" -eq 1 ]; then
    echo "[i] Removing ${BUILD_DIR}"
    # Tolerant: never let cleanup fail the run (root-owned leftovers from a
    # prior sudo --install, missing dir, etc.). Try sudo, then ignore.
    rm -rf "${BUILD_DIR}" 2>/dev/null || sudo rm -rf "${BUILD_DIR}" 2>/dev/null || true
fi

mkdir -p "${BUILD_DIR}"

CMAKE_ARGS=(
    "-DCMAKE_BUILD_TYPE=${BUILD_TYPE}"
    "-DWITH_STUBS=${WITH_STUBS}"
    # Always pass the prefix explicitly so a stale value cached in an existing
    # build dir from a previous --prefix run can never silently win.
    "-DCMAKE_INSTALL_PREFIX=${PREFIX}"
)

echo "[i] Configuring: cmake ${CMAKE_ARGS[*]}"
cmake -S "${REPO_ROOT}" -B "${BUILD_DIR}" "${CMAKE_ARGS[@]}"

echo "[i] Building libnvmpi with ${JOBS} jobs"
cmake --build "${BUILD_DIR}" -j "${JOBS}"

if [ "${DO_PACKAGE}" -eq 1 ]; then
    echo "[i] Building .deb package (CPack)"
    cmake --build "${BUILD_DIR}" --target package
    echo "[i] Package: $(ls "${BUILD_DIR}"/*.deb 2>/dev/null)"
fi

if [ "${DO_INSTALL}" -eq 1 ]; then
    SUDO=""
    [ "$(id -u)" -ne 0 ] && SUDO="sudo"
    echo "[i] Installing libnvmpi to ${PREFIX}"
    ${SUDO} cmake --install "${BUILD_DIR}"
    ${SUDO} ldconfig || true
fi

echo "[i] Done. libnvmpi artifacts in ${BUILD_DIR}"
