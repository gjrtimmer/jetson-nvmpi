#!/usr/bin/env bash
# Clean-system install test for the libnvmpi Debian packages.
#
# Run as root inside a CLEAN ubuntu container (22.04 for JP6, 20.04 for JP5),
# NOT the builder image: the point is to prove the packages' dependencies
# resolve from NVIDIA's apt repo alone and that the installed library works.
#
# Steps (each prints PASS or fails the script):
#   1  apt-get install libnvmpi1 alone: dependency resolution
#   2  ldconfig lists libnvmpi.so.1; unversioned libnvmpi.so absent (runtime pkg)
#   3  JP6: dlopen("libnvmpi.so.1") + dlsym of the codec entry points (the
#      jetson-ffmpeg runtime scenario; no unversioned symlink available)
#      JP5: static equivalent (readelf: entry points defined). NVIDIA's r35.4
#      libs abort in a constructor without /dev/nvmap, so nothing is loaded.
#   4  install libnvmpi-dev: libnvmpi.so symlink, nvmpi.h, nvmpi.pc; compile
#      and link (pkg-config) a program calling one symbol, ldd clean;
#      JP6 also runs it (JP5: not run, same constructor abort)
#   5  purge both: no file the packages owned remains, ldconfig clean
#   +  lintian on both debs (--lintian): E: tags fail, other tags are reported
#
# Usage:
#   scripts/test-deb.sh --jp 6|5 --debdir DIR [--lintian] [--force]
#
#   --jp N        JetPack major: 6 (L4T r36.4, t234) or 5 (L4T r35.4, t194)
#   --debdir DIR  Directory with libnvmpi1_*jp<N>_arm64.deb and
#                 libnvmpi-dev_*jp<N>_arm64.deb (read-only is fine; both the
#                 '~jpN' CI names and GitHub's '.jpN' renames match)
#   --lintian     Also run lintian (installs it; only E: tags fail)
#   --force       Run outside a container (installs/purges packages for real)
#
# Idempotent: re-running reuses the apt source and ends with the packages purged.
set -euo pipefail

JP=""; DEBDIR=""; LINTIAN=0; FORCE=0
usage() { sed -n '2,/^set /{/^set /d;s/^# \{0,1\}//;p}' "${BASH_SOURCE[0]}"; }
while [ $# -gt 0 ]; do
    case "$1" in
        --jp)      JP="$2"; shift ;;
        --debdir)  DEBDIR="$2"; shift ;;
        --lintian) LINTIAN=1 ;;
        --force)   FORCE=1 ;;
        -h|--help) usage; exit 0 ;;
        *) echo "[E] unknown option: $1" >&2; usage; exit 1 ;;
    esac
    shift
done
case "${JP}" in
    6) L4T="r36.4"; SOC="t234" ;;
    5) L4T="r35.4"; SOC="t194" ;;
    *) echo "[E] --jp must be 6 or 5" >&2; usage; exit 1 ;;
esac
[ -d "${DEBDIR}" ] || { echo "[E] --debdir is required and must exist" >&2; exit 1; }
[ "$(id -u)" -eq 0 ] || { echo "[E] must run as root" >&2; exit 1; }
if [ "${FORCE}" -ne 1 ] && [ ! -f /.dockerenv ] && [ -z "${CI:-}" ]; then
    echo "[E] not in a container or CI: refusing to install/purge packages (use --force)" >&2
    exit 1
fi

LIBDIR="/usr/lib/aarch64-linux-gnu"
WORK="$(mktemp -d)"
trap 'rm -rf "${WORK}"' EXIT
export DEBIAN_FRONTEND=noninteractive
ok() { echo "[PASS] $*"; }
die() { echo "[FAIL] $*" >&2; exit 1; }

# Same L4T workarounds as the dist jobs: the touch file skips
# nvidia-l4t-core's "must run on an NVIDIA Tegra platform" preinstall check.
mkdir -p /opt/nvidia/l4t-packages
touch /opt/nvidia/l4t-packages/.nv-l4t-disable-boot-fw-update-in-preinstall

apt-get update -qq
# Some CI runners mount their own CA bundle read-only over /etc/ssl, which
# makes dpkg fail on the openssl/ca-certificates packages: only install
# ca-certificates when the image has no CA bundle.
PKGS=(wget gcc libc6-dev pkg-config binutils)
[ -s /etc/ssl/certs/ca-certificates.crt ] || PKGS+=(ca-certificates)
apt-get install -y -qq --no-install-recommends "${PKGS[@]}"
if ! grep -rqs "repo.download.nvidia.com/jetson/${SOC}" /etc/apt/sources.list /etc/apt/sources.list.d/; then
    wget -O /etc/apt/trusted.gpg.d/jetson-ota-public.asc https://repo.download.nvidia.com/jetson/jetson-ota-public.asc || {
        echo "[E] fetching the NVIDIA apt key failed; CA setup:" >&2
        grep -E ' /etc/ssl' /proc/mounts >&2 || true
        ls -ld /etc/ssl /etc/ssl/certs /etc/ssl/certs/ca-certificates.crt >&2 || true
        grep -c 'BEGIN CERTIFICATE' /etc/ssl/certs/ca-certificates.crt >&2 || true
        exit 1
    }
    {
        echo "deb https://repo.download.nvidia.com/jetson/common ${L4T} main"
        echo "deb https://repo.download.nvidia.com/jetson/${SOC} ${L4T} main"
    } > /etc/apt/sources.list.d/nvidia-l4t.list
fi
apt-get update -qq

# Copy the debs out of the (read-only) mount so apt reads them as root without
# the _apt sandbox warning.
mkdir -p "${WORK}/debs"
RT="$(ls "${DEBDIR}"/libnvmpi1_*jp"${JP}"_arm64.deb)"
DEV="$(ls "${DEBDIR}"/libnvmpi-dev_*jp"${JP}"_arm64.deb)"
cp "${RT}" "${DEV}" "${WORK}/debs/"
RT="${WORK}/debs/$(basename "${RT}")"; DEV="${WORK}/debs/$(basename "${DEV}")"
VER="$(dpkg-deb -f "${RT}" Version)"
echo "[i] JP${JP}: libnvmpi1 ${VER} ($(dpkg-deb -f "${RT}" Depends))"

# 1 ---------------------------------------------------------------------
apt-get install -y --no-install-recommends "${RT}" || die "1: libnvmpi1 install/dependency resolution failed"
dpkg -s libnvmpi1 | grep -q '^Status: install ok installed' || die "1: libnvmpi1 not installed"
dpkg -s nvidia-l4t-multimedia | grep -q '^Status: install ok installed' || die "1: nvidia-l4t-multimedia not pulled in"
ok "1 install libnvmpi1 alone, dependencies resolved"

# 2 ---------------------------------------------------------------------
ldconfig
ldconfig -p | grep -F 'libnvmpi.so.1' || die "2: ldconfig lacks libnvmpi.so.1"
if ldconfig -p | grep -Eq 'libnvmpi\.so( |$)' || [ -e "${LIBDIR}/libnvmpi.so" ] || [ -L "${LIBDIR}/libnvmpi.so" ]; then
    die "2: unversioned libnvmpi.so present with only the runtime package installed"
fi
if ldd "${LIBDIR}/libnvmpi.so.1" | grep -F 'not found'; then die "2: unresolved NEEDED libs"; fi
ok "2 ldconfig has libnvmpi.so.1, no unversioned libnvmpi.so"

# 3 ---------------------------------------------------------------------
cat > "${WORK}/dl.c" <<'CEOF'
#include <dlfcn.h>
#include <stdio.h>
int main(void) {
    void *h = dlopen("libnvmpi.so.1", RTLD_NOW);
    if (!h) { fprintf(stderr, "dlopen: %s\n", dlerror()); return 1; }
    const char *syms[] = { "nvmpi_create_decoder", "nvmpi_create_encoder" };
    for (int i = 0; i < 2; i++) {
        if (!dlsym(h, syms[i])) { fprintf(stderr, "dlsym %s: %s\n", syms[i], dlerror()); return 2; }
        printf("resolved %s\n", syms[i]);
    }
    return dlclose(h) ? 3 : 0;
}
CEOF
if [ "${JP}" = "6" ]; then
    gcc -o "${WORK}/dl" "${WORK}/dl.c" -ldl
    "${WORK}/dl" || die "3: dlopen/dlsym of libnvmpi.so.1 failed"
    ok "3 dlopen(libnvmpi.so.1) + dlsym codec entry points"
else
    syms="$(readelf --dyn-syms -W "${LIBDIR}/libnvmpi.so.1")"
    for s in nvmpi_create_decoder nvmpi_create_encoder; do
        grep -Eq " FUNC .* DEFAULT +[0-9]+ ${s}\$" <<<"${syms}" || die "3: ${s} not exported by libnvmpi.so.1"
    done
    ok "3 (static) libnvmpi.so.1 exports the codec entry points"
fi

# 4 ---------------------------------------------------------------------
apt-get install -y --no-install-recommends "${DEV}" || die "4: libnvmpi-dev install failed"
[ -L "${LIBDIR}/libnvmpi.so" ] || die "4: libnvmpi.so symlink missing"
[ "$(readlink "${LIBDIR}/libnvmpi.so")" = "libnvmpi.so.1" ] || die "4: libnvmpi.so does not point at libnvmpi.so.1"
[ -f /usr/include/nvmpi.h ] || die "4: nvmpi.h missing"
[ -f "${LIBDIR}/pkgconfig/nvmpi.pc" ] || die "4: nvmpi.pc missing"
PCVER="$(pkg-config --modversion nvmpi)"
case "${VER}" in "${PCVER}"-*) ;; *) die "4: nvmpi.pc version ${PCVER} != package ${VER}" ;; esac
cat > "${WORK}/link.c" <<'CEOF'
#include <stdio.h>
#include <nvmpi.h>
int main(void) {
    /* Safe without hardware: NULL context is rejected before any device access. */
    int r = nvmpi_decoder_close(NULL);
    printf("nvmpi_decoder_close(NULL) = %d\n", r);
    return r == -1 ? 0 : 1;
}
CEOF
# shellcheck disable=SC2046 # pkg-config output must word-split
gcc -o "${WORK}/link" "${WORK}/link.c" $(pkg-config --cflags --libs nvmpi)
if ldd "${WORK}/link" | grep -F 'not found'; then die "4: linked program has unresolved libs"; fi
if [ "${JP}" = "6" ]; then
    "${WORK}/link" || die "4: linked test program failed"
fi
ok "4 libnvmpi-dev: symlink, header, pc; compile+link via pkg-config (JP6: run)"

# lintian (report only) ---------------------------------------------------
if [ "${LINTIAN}" -eq 1 ]; then
    if apt-get install -y -qq --no-install-recommends lintian >/dev/null 2>&1; then
        lintian --no-tag-display-limit "${RT}" "${DEV}" | tee "${WORK}/lintian.txt" || true
        if grep -q '^E: ' "${WORK}/lintian.txt"; then die "lintian: errors found"; fi
        ok "lintian: no errors"
    else
        echo "[W] lintian not installable here, skipped"
    fi
fi

# 5 ---------------------------------------------------------------------
# Non-directory paths the packages own, captured before the purge.
dpkg -L libnvmpi1 libnvmpi-dev | grep -v '^/\.$' | while read -r p; do
    if [ -d "${p}" ] && [ ! -L "${p}" ]; then continue; fi
    echo "${p}"
done > "${WORK}/owned.txt"
[ -s "${WORK}/owned.txt" ] || die "5: package file list is empty"
apt-get purge -y libnvmpi-dev libnvmpi1 >/dev/null
ldconfig
left=0
while read -r p; do
    if [ -e "${p}" ] || [ -L "${p}" ]; then echo "[E] left behind: ${p}" >&2; left=1; fi
done < "${WORK}/owned.txt"
[ "${left}" -eq 0 ] || die "5: files remain after purge"
if ls /usr/share/doc/libnvmpi* "${LIBDIR}"/libnvmpi* >/dev/null 2>&1; then die "5: libnvmpi paths remain"; fi
if ldconfig -p | grep -F libnvmpi; then die "5: ldconfig cache still lists libnvmpi"; fi
dpkg -l 'libnvmpi*' 2>&1 | grep -E '^(ii|rc)' && die "5: package state remains (ii/rc)"
ok "5 purge: no leftover files ($(wc -l < "${WORK}/owned.txt") checked), ldconfig clean"
echo "[i] all steps passed for JP${JP}"
