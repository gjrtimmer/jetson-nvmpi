#!/usr/bin/env bash
# Upload the dist/*.tar.gz archives and dist/*.deb packages (and optionally a changelog) to S3 (MinIO).
# Intended for the CI "upload:s3" job; every external dependency is an env var
# so it can be dry-run. Uses plain curl SigV4 (needs curl >= 7.75), no mc.
#
# Usage:
#   scripts/upload-s3.sh <dest-prefix-under-bucket> [changelog-file]
#   e.g. releases/<ver>  or  snapshots/<short-sha>
#
# Layout: <dest-prefix>/jp<N>/<archive> and <dest-prefix>/CHANGELOG.md
#
# Required (project CI/CD variables; missing = hard failure):
#   S3_HOST, S3_ACCESS_KEY, S3_SECRET_KEY, S3_BUCKET
# Optional: S3_REGION (default us-east-1)
set -euo pipefail

# One folder per JetPack, derived from the L4T major in the archive name
# (package.sh: ..._l4tr<major>.<x>.<y>_...) or the ~jp<N> deb version suffix.
jetpack_dir() {
    case "$1" in
        *_l4tr36.*|*~jp6_*.deb) echo jp6 ;;
        *_l4tr35.*|*~jp5_*.deb) echo jp5 ;;
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
    local archives=( "${repo_root}"/dist/*.tar.gz "${repo_root}"/dist/*.deb )
    if [ ${#archives[@]} -eq 0 ]; then echo "[E] no archives in ${repo_root}/dist" >&2; exit 1; fi
    if [ -n "${changelog}" ] && [ ! -f "${changelog}" ]; then
        echo "[E] changelog file not found: ${changelog}" >&2; exit 1
    fi

    : "${S3_HOST:?S3_HOST is required}"
    : "${S3_ACCESS_KEY:?S3_ACCESS_KEY is required}"
    : "${S3_SECRET_KEY:?S3_SECRET_KEY is required}"
    : "${S3_BUCKET:?S3_BUCKET is required}"

    check_curl

    local base="${S3_HOST%/}/${S3_BUCKET}"
    echo "[i] uploading ${#archives[@]} archives to ${S3_BUCKET}/${dest}/jp*/"
    local f name jp
    for f in "${archives[@]}"; do
        name="$(basename "$f")"
        jp="$(jetpack_dir "$name")"
        s3_put "$f" "${base}" "${dest}/${jp}/${name}"
    done

    if [ -n "${changelog}" ]; then
        s3_put "${changelog}" "${base}" "${dest}/CHANGELOG.md"
    fi
}

# Fail fast when curl cannot sign requests (--aws-sigv4 arrived in 7.75).
check_curl() {
    command -v curl >/dev/null 2>&1 || { echo "[E] curl not found" >&2; return 1; }
    if ! curl --help all 2>/dev/null | grep -q aws-sigv4; then
        echo "[E] curl lacks --aws-sigv4 (need curl >= 7.75): $(curl --version | head -1)" >&2
        return 1
    fi
}

# Signed curl call against the S3 endpoint. Credentials only go to curl.
s3_curl() {
    curl --fail --silent --show-error \
        --aws-sigv4 "aws:amz:${S3_REGION:-us-east-1}:s3" \
        --user "${S3_ACCESS_KEY}:${S3_SECRET_KEY}" "$@"
}

# s3_put <file> <base-url-with-bucket> <key>: PUT then verify with signed HEAD.
s3_put() {
    local file="$1" base="$2" key="$3" size
    # Keys go into the URL unencoded, so only allow a safe charset.
    if [[ ! "${key}" =~ ^[A-Za-z0-9._~/-]+$ ]]; then
        echo "[E] unsafe object key: ${key}" >&2; return 1
    fi
    echo "  -> ${key}"
    s3_curl --upload-file "${file}" "${base}/${key}"
    size="$(s3_curl -I "${base}/${key}" | tr -d '\r' | awk 'tolower($1)=="content-length:" {print $2}')"
    echo "[i] verified ${key} (${size:-unknown} bytes)"
}

# Allow sourcing (dry-check of jetpack_dir) without running.
if [ "${BASH_SOURCE[0]}" = "$0" ]; then main "$@"; fi
