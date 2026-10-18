#!/usr/bin/env bash
# Build and verify the Debian packages (libnvmpi1 + libnvmpi-dev) with CPack:
#   <outdir>/libnvmpi1_<debver>_arm64.deb
#   <outdir>/libnvmpi-dev_<debver>_arm64.deb
#
# Uses a dedicated build tree (build-deb) configured with prefix /usr and the
# multiarch libdir, so the packaged nvmpi.pc and file paths are correct.
# Needs the real Jetson libs (stubs are refused); run after the same setup as
# scripts/build.sh --no-stubs.
#
# Usage:
#   scripts/package-deb.sh --version DEBVER --jp 6|5 [options]
#
#   --version DEBVER  Full deb version, e.g. 4.0.0-1~jp6 (required)
#   --jp N            JetPack major, 6 or 5; selects the L4T pin 36 / 35 (required)
#   --outdir DIR      Output directory (default: <repo>/dist)
#   --build-dir DIR   Build directory (default: <repo>/build-deb)
#   -j N              Parallel build jobs (default: nproc)
#   -h, --help        Show this help
#
# Safe to run from any working directory.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

DEBVER=""
JP=""
OUTDIR="${REPO_ROOT}/dist"
BUILD_DIR="${REPO_ROOT}/build-deb"
JOBS="$(nproc)"

usage() { sed -n '2,/^set /{/^set /d;s/^# \{0,1\}//;p}' "${BASH_SOURCE[0]}"; }

while [ $# -gt 0 ]; do
    case "$1" in
        --version)   DEBVER="$2"; shift ;;
        --jp)        JP="$2"; shift ;;
        --outdir)    OUTDIR="$2"; shift ;;
        --build-dir) BUILD_DIR="$2"; shift ;;
        -j)          JOBS="$2"; shift ;;
        -j*)         JOBS="${1#-j}" ;;
        -h|--help)   usage; exit 0 ;;
        *) echo "[E] unknown option: $1" >&2; usage; exit 1 ;;
    esac
    shift
done

[ -n "${DEBVER}" ] || { echo "[E] --version is required" >&2; usage; exit 1; }
case "${JP}" in
    6) L4T_MAJOR=36 ;;
    5) L4T_MAJOR=35 ;;
    *) echo "[E] --jp must be 6 or 5" >&2; usage; exit 1 ;;
esac

echo "[i] configuring ${BUILD_DIR} (deb ${DEBVER}, L4T pin ${L4T_MAJOR})"
cmake -S "${REPO_ROOT}" -B "${BUILD_DIR}" \
    -DCMAKE_BUILD_TYPE=Release -DWITH_STUBS=OFF \
    -DCMAKE_INSTALL_PREFIX=/usr -DCMAKE_INSTALL_LIBDIR=lib/aarch64-linux-gnu \
    -DNVMPI_DEB_VERSION="${DEBVER}" -DNVMPI_DEB_L4T_MAJOR="${L4T_MAJOR}"
cmake --build "${BUILD_DIR}" -j "${JOBS}"

# Fresh package dir so stale debs from a previous run never leak into dist/.
PKGDIR="${BUILD_DIR}/deb-out"
rm -rf "${PKGDIR}"
mkdir -p "${PKGDIR}" "${OUTDIR}"
(cd "${BUILD_DIR}" && cpack -G DEB -B "${PKGDIR}")

RUNTIME_DEB="$(ls "${PKGDIR}"/libnvmpi1_*.deb)"
DEV_DEB="$(ls "${PKGDIR}"/libnvmpi-dev_*.deb)"

# Verify: print metadata + contents, then assert the files that matter.
for deb in "${RUNTIME_DEB}" "${DEV_DEB}"; do
    echo "=== $(basename "${deb}") ==="
    dpkg-deb -I "${deb}"
    dpkg-deb -c "${deb}"
done
# Capture output before grepping: grep -q exits on first match, dpkg-deb then
# dies of SIGPIPE and pipefail turns a match into a failure.
check() { # <deb> <extended-regex> <what>
    local listing
    listing="$(dpkg-deb -c "$1")"
    grep -Eq "$2" <<<"${listing}" || { echo "[E] $(basename "$1") lacks $3" >&2; exit 1; }
}
check "${RUNTIME_DEB}" ' \./usr/lib/aarch64-linux-gnu/libnvmpi\.so\.1( |$)' "libnvmpi.so.1"
check "${DEV_DEB}" ' \./usr/lib/aarch64-linux-gnu/libnvmpi\.so -> ' "libnvmpi.so symlink"
check "${DEV_DEB}" ' \./usr/include/nvmpi\.h$' "nvmpi.h"
check "${DEV_DEB}" ' \./usr/lib/aarch64-linux-gnu/pkgconfig/nvmpi\.pc$' "nvmpi.pc"
grep -Fq "libnvmpi1 (= ${DEBVER})" <<<"$(dpkg-deb -f "${DEV_DEB}" Depends)" \
    || { echo "[E] libnvmpi-dev does not pin libnvmpi1 (= ${DEBVER})" >&2; exit 1; }

cp -f "${RUNTIME_DEB}" "${DEV_DEB}" "${OUTDIR}/"
echo "[i] done: ${OUTDIR}"
