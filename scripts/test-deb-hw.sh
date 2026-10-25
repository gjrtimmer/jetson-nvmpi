#!/usr/bin/env bash
# Hardware test of the INSTALLED release debs (JP6) on a real Jetson.
#
# Run as root in the hw runtime image (tegra libs injected by the NVIDIA
# runtime). Installs libnvmpi1 + libnvmpi-dev from DEBDIR, builds test/api
# test_api against the installed library via pkg-config only (no build tree,
# no -L), asserts libnvmpi resolves from the system path and runs test_api.
# Complements test-deb.sh (clean container, no hardware): this proves the
# packaged library drives the real codecs. Purges the packages afterwards.
#
# Usage: scripts/test-deb-hw.sh --debdir DIR
set -euo pipefail

DEBDIR=""
while [ $# -gt 0 ]; do
    case "$1" in
        --debdir) DEBDIR="$2"; shift ;;
        -h|--help) sed -n '2,/^set /{/^set /d;s/^# \{0,1\}//;p}' "${BASH_SOURCE[0]}"; exit 0 ;;
        *) echo "[E] unknown option: $1" >&2; exit 1 ;;
    esac
    shift
done
[ -d "${DEBDIR}" ] || { echo "[E] --debdir is required and must exist" >&2; exit 1; }
[ "$(id -u)" -eq 0 ] || { echo "[E] must run as root" >&2; exit 1; }

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WORK="$(mktemp -d)"
export DEBIAN_FRONTEND=noninteractive
cleanup() {
    dpkg --purge libnvmpi-dev libnvmpi1 >/dev/null 2>&1 || true
    rm -rf "${WORK}"
}
trap cleanup EXIT

# Compiler + pkg-config only when the image lacks them (no tegra libs touched).
RT="$(ls "${DEBDIR}"/libnvmpi1_*jp6_arm64.deb)"
DEV="$(ls "${DEBDIR}"/libnvmpi-dev_*jp6_arm64.deb)"
cp "${RT}" "${DEV}" "${WORK}/"
RT="${WORK}/$(basename "${RT}")"
DEV="${WORK}/$(basename "${DEV}")"

apt-get update -qq
if ! command -v gcc >/dev/null || ! command -v pkg-config >/dev/null; then
    apt-get install -y -qq --no-install-recommends gcc libc6-dev pkg-config
fi

if dpkg-query -W -f='${Status}' nvidia-l4t-multimedia 2>/dev/null \
        | grep -q 'install ok installed'; then
    apt-get install -y --no-install-recommends "${RT}" "${DEV}"
else
    # The NVIDIA runtime bind-mounts the host tegra libs without registering
    # them with dpkg, and apt cannot install nvidia-l4t-* over the mounts
    # ("Invalid cross-device link"). Install the other runtime deps with apt,
    # then force only the unregistered tegra dependency. Dependency resolution
    # itself is covered by test:deb:jp6; the ldd check below still fails on
    # any library that is really missing.
    apt-get install -y -qq --no-install-recommends libjpeg-turbo8 libstdc++6
    echo "[i] nvidia-l4t-multimedia not dpkg-registered (runtime-injected): dpkg --force-depends"
    dpkg -i --force-depends "${RT}" "${DEV}"
fi
rm -rf /var/lib/apt/lists/*
ldconfig

# Injected tegra libs may sit outside the container's ld.so.conf (same dirs
# test:api:hw uses, minus the build tree): libnvmpi itself must still come
# from the package's system path, which the ldd check below asserts.
export LD_LIBRARY_PATH="/usr/lib/aarch64-linux-gnu/nvidia:/usr/lib/aarch64-linux-gnu/tegra"

# Only the pkg-config flags plus the NVIDIA Multimedia API: nothing from a
# build tree can sneak in. The odd-size put_frame_fd case (#156) allocates a
# real NvBufSurface, so it needs nvbufsurface.h and -lnvbufsurface, as the
# in-tree build gets them from CMakeLists.txt. NVMPI_REQUIRE_SURF turns a
# missing header into a compile error instead of a case that cannot run.
MM_API="${JETSON_MULTIMEDIA_API_DIR:-/usr/src/jetson_multimedia_api}"
[ -f "${MM_API}/include/nvbufsurface.h" ] \
    || { echo "[FAIL] ${MM_API}/include/nvbufsurface.h missing (set JETSON_MULTIMEDIA_API_DIR)" >&2; exit 1; }
# shellcheck disable=SC2046 # pkg-config output must word-split
gcc -O1 -DNVMPI_REQUIRE_SURF -o "${WORK}/test_api" "${ROOT}/test/api/test_api.c" \
    $(pkg-config --cflags --libs nvmpi) -I"${MM_API}/include" \
    -L/usr/lib/aarch64-linux-gnu/nvidia -L/usr/lib/aarch64-linux-gnu/tegra -lnvbufsurface -pthread -lm

ldd "${WORK}/test_api" | tee "${WORK}/ldd.txt"
! grep -F 'not found' "${WORK}/ldd.txt" || { echo "[FAIL] unresolved libs" >&2; exit 1; }
grep -Eq 'libnvmpi\.so\.1 => /(usr/)?lib/' "${WORK}/ldd.txt" \
    || { echo "[FAIL] libnvmpi.so.1 does not resolve from the system lib path" >&2; exit 1; }
echo "[PASS] libnvmpi.so.1 resolves from the installed package"

"${WORK}/test_api"
echo "[PASS] test_api on hardware against the installed debs"
