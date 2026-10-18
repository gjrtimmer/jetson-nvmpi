#!/usr/bin/env bash
# Upload the dist/*.tar.gz archives (and optionally a changelog) to S3 (MinIO).
# Intended for the CI "upload:s3" job; every external dependency is an env var
# so it can be dry-run. Requires the `mc` (MinIO client) binary.
#
# Usage:
#   scripts/upload-s3.sh <dest-prefix-under-bucket> [changelog-file]
#   e.g. releases/<ver>  or  snapshots/<short-sha>
#
# Layout: <dest-prefix>/jp<N>/<archive> and <dest-prefix>/CHANGELOG.md
#
# Required (project CI/CD variables; missing = hard failure):
#   S3_HOST, S3_ACCESS_KEY, S3_SECRET_KEY, S3_BUCKET
set -euo pipefail

# One folder per JetPack, derived from the L4T major in the archive name
# (package.sh: ..._l4tr<major>.<x>.<y>_...).
jetpack_dir() {
    case "$1" in
        *_l4tr36.*) echo jp6 ;;
        *_l4tr35.*) echo jp5 ;;
        *) echo "[E] no JetPack mapping for $1" >&2; return 1 ;;
    esac
}

main() {
    local dest="${1:?usage: upload-s3.sh <dest-prefix> [changelog-file]}"
    local changelog="${2:-}"
    local script_dir repo_root
    script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
    repo_root="$(cd "${script_dir}/.." && pwd)"
    cd "${repo_root}"

    shopt -s nullglob
    local archives=( "${repo_root}"/dist/*.tar.gz )
    if [ ${#archives[@]} -eq 0 ]; then echo "[E] no archives in ${repo_root}/dist" >&2; exit 1; fi
    if [ -n "${changelog}" ] && [ ! -f "${changelog}" ]; then
        echo "[E] changelog file not found: ${changelog}" >&2; exit 1
    fi

    : "${S3_HOST:?S3_HOST is required}"
    : "${S3_ACCESS_KEY:?S3_ACCESS_KEY is required}"
    : "${S3_SECRET_KEY:?S3_SECRET_KEY is required}"
    : "${S3_BUCKET:?S3_BUCKET is required}"

    # Determine secure/insecure based on endpoint scheme
    local mc_secure="true"
    if [[ "${S3_HOST}" == http://* ]]; then
        mc_secure="false"
        echo "[i] S3 endpoint uses http — insecure mode (in-cluster)"
    fi

    # Configure mc alias for this pipeline run
    local alias_flags=(--api S3v4 --path auto)
    [ "${mc_secure}" = "false" ] && alias_flags+=(--insecure)
    mc alias set s3 "${S3_HOST}" "${S3_ACCESS_KEY}" "${S3_SECRET_KEY}" \
        "${alias_flags[@]}" 2>/dev/null

    local s3_prefix="s3/${S3_BUCKET}/${dest}"
    local mc_flags=()
    [ "${mc_secure}" = "false" ] && mc_flags+=(--insecure)

    echo "[i] uploading ${#archives[@]} archives to ${s3_prefix}/jp*/"
    local f name jp
    for f in "${archives[@]}"; do
        name="$(basename "$f")"
        jp="$(jetpack_dir "$name")"
        echo "  -> ${jp}/${name}"
        mc cp "${mc_flags[@]}" "$f" "${s3_prefix}/${jp}/${name}"
    done

    if [ -n "${changelog}" ]; then
        echo "[i] uploading CHANGELOG.md to ${s3_prefix}/"
        mc cp "${mc_flags[@]}" "${changelog}" "${s3_prefix}/CHANGELOG.md"
    fi

    echo "[i] verifying S3 upload"
    mc ls --recursive "${mc_flags[@]}" "${s3_prefix}/" | head -20
}

# Allow sourcing (dry-check of jetpack_dir) without running.
if [ "${BASH_SOURCE[0]}" = "$0" ]; then main "$@"; fi
