#!/usr/bin/env bash
# Package an installed libnvmpi prefix into a distributable archive:
#   <outdir>/jetson-nvmpi_<version>_l4t<l4t>_<arch>.tar.gz
#
# Contents (install: tar xzf <archive> --strip-components=1 -C /usr/local, then ldconfig):
#   lib/libnvmpi.so.<ver>, lib/libnvmpi.so.<soversion>, lib/libnvmpi.so
#   include/*.h                (public headers installed by CMake)
#   lib/pkgconfig/nvmpi.pc     (prefix rewritten to /usr/local)
#
# The unversioned lib/libnvmpi.so symlink is REQUIRED: FFmpeg dlopens
# "libnvmpi.so". Stub libraries are never shipped.
#
# Usage:
#   scripts/package.sh --prefix DIR [options]
#
#   --prefix DIR   Install prefix produced by `scripts/build.sh --install` (required)
#   --version VER  Package version (default: project VERSION from CMakeLists.txt)
#   --l4t TAG      L4T/JetPack tag (default: $L4T_TAG or r36.4.0)
#   --arch ARCH    Architecture label (default: aarch64)
#   --outdir DIR   Output directory (default: <repo>/dist)
#   -h, --help     Show this help
#
# Safe to run from any working directory.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

PREFIX=""
VERSION=""
L4T="${L4T_TAG:-r36.4.0}"
ARCH="aarch64"
OUTDIR="${REPO_ROOT}/dist"

usage() { sed -n '2,/^set /{/^set /d;s/^# \{0,1\}//;p}' "${BASH_SOURCE[0]}"; }

while [ $# -gt 0 ]; do
    case "$1" in
        --prefix)  PREFIX="$2"; shift ;;
        --version) VERSION="$2"; shift ;;
        --l4t)     L4T="$2"; shift ;;
        --arch)    ARCH="$2"; shift ;;
        --outdir)  OUTDIR="$2"; shift ;;
        -h|--help) usage; exit 0 ;;
        *) echo "[E] unknown option: $1" >&2; usage; exit 1 ;;
    esac
    shift
done

[ -n "${PREFIX}" ] || { echo "[E] --prefix is required" >&2; usage; exit 1; }
[ -d "${PREFIX}" ] || { echo "[E] prefix not found: ${PREFIX}" >&2; exit 1; }

if [ -z "${VERSION}" ]; then
    VERSION="$(sed -n 's/^project(nvmpi VERSION \([0-9][0-9.]*\).*/\1/p' "${REPO_ROOT}/CMakeLists.txt")"
    [ -n "${VERSION}" ] || { echo "[E] cannot read VERSION from CMakeLists.txt" >&2; exit 1; }
fi

NAME="jetson-nvmpi_${VERSION}_l4t${L4T}_${ARCH}"
STAGE="$(mktemp -d)"
DEST="${STAGE}/${NAME}"
mkdir -p "${DEST}/lib/pkgconfig" "${DEST}/include"

# Library dir may be lib/ or lib/<multiarch>/ depending on GNUInstallDirs.
LIBDIR="$(dirname "$(find "${PREFIX}" -name 'libnvmpi.so' \( -type l -o -type f \) | head -n1)")"
if [ ! -d "${LIBDIR}" ] || [ ! -e "${LIBDIR}/libnvmpi.so" ]; then
    echo "[E] libnvmpi.so not found under ${PREFIX}" >&2; rm -rf "${STAGE}"; exit 1
fi

echo "[i] staging ${LIBDIR} -> ${NAME}"
# cp -a keeps the versioned file and the .so.<soversion> / .so symlinks intact.
cp -a "${LIBDIR}"/libnvmpi.so* "${DEST}/lib/"
cp -a "${PREFIX}"/include/*.h "${DEST}/include/"
test -L "${DEST}/lib/libnvmpi.so" || [ -f "${DEST}/lib/libnvmpi.so" ] \
    || { echo "[E] unversioned libnvmpi.so missing from stage" >&2; exit 1; }

PC="$(find "${PREFIX}" -name nvmpi.pc | head -n1)"
[ -n "${PC}" ] || { echo "[E] nvmpi.pc not found under ${PREFIX}" >&2; exit 1; }
sed "s|^prefix=.*|prefix=/usr/local|;s|^exec_prefix=.*|exec_prefix=/usr/local|;s|^libdir=.*|libdir=\${exec_prefix}/lib|" \
    "${PC}" > "${DEST}/lib/pkgconfig/nvmpi.pc"

mkdir -p "${OUTDIR}"
TARBALL="${OUTDIR}/${NAME}.tar.gz"
echo "[i] writing ${TARBALL}"
tar -C "${STAGE}" -czf "${TARBALL}" "${NAME}"
# Cleanup must never fail the job once the archive is written.
rm -rf "${STAGE}" 2>/dev/null || true

echo "[i] done: ${TARBALL}"
