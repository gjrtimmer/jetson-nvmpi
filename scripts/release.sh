#!/usr/bin/env bash
# Create the release: GitLab generic package registry -> GitLab Release.
# S3 upload and changelog are separate jobs (upload-s3.sh, changelog.sh).
# Intended to run inside the release-tools image on a tag pipeline
# (see .gitlab-ci.yml "release:gitlab" job). Not meant for local use, but every
# external dependency is an env var so it can be dry-run.
#
# Required (provided by GitLab CI on a tag pipeline):
#   CI_COMMIT_TAG, CI_API_V4_URL, CI_PROJECT_ID, CI_JOB_TOKEN
# Inputs:
#   dist/*.tar.gz - archives produced by scripts/package.sh
#   dist/*.deb    - packages produced by scripts/package-deb.sh
#   NOTES.md      - this version's notes (scripts/changelog.sh); release description
#   CHANGELOG.md  - full changelog (repo root); attached as an asset, NOTES.md if absent
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
cd "${REPO_ROOT}"

TAG="${CI_COMMIT_TAG:?CI_COMMIT_TAG is required (run on a tag pipeline)}"
VERSION="${TAG#v}"
DIST="${REPO_ROOT}/dist"

echo "=== releasing ${TAG} (version ${VERSION}) ==="

# --- 1. inputs -----------------------------------------------------------
# NOTES.md comes from the upload:s3 job (scripts/changelog.sh) as an artifact.
[ -f NOTES.md ] || { echo "[E] NOTES.md missing (upload:s3 artifact)" >&2; exit 1; }
shopt -s nullglob
ARCHIVES=( "${DIST}"/*.tar.gz "${DIST}"/*.deb )
if [ ${#ARCHIVES[@]} -eq 0 ]; then echo "[E] no archives in ${DIST}" >&2; exit 1; fi

# --- 2. upload archives to GitLab generic package registry ---------------

ASSET_ARGS=()
PKG_BASE="${CI_API_V4_URL}/projects/${CI_PROJECT_ID}/packages/generic/jetson-nvmpi/${VERSION}"
for f in "${ARCHIVES[@]}"; do
    name="$(basename "$f")"
    url="${PKG_BASE}/${name}"
    echo "[i] uploading ${name} -> generic package registry"
    curl --fail --silent --show-error \
         --header "JOB-TOKEN: ${CI_JOB_TOKEN}" \
         --upload-file "$f" "$url"
    ASSET_ARGS+=( --assets-link "{\"name\":\"${name}\",\"url\":\"${url}\",\"link_type\":\"package\"}" )
done

# --- 2b. changelog asset ---------------------------------------------------
# Attach the full CHANGELOG.md (maintained by the release commit; present in
# the tag checkout). Fall back to this version's NOTES.md when it is missing.
CHANGELOG_SRC="CHANGELOG.md"
[ -f "${CHANGELOG_SRC}" ] || CHANGELOG_SRC="NOTES.md"
CL_URL="${PKG_BASE}/CHANGELOG.md"
echo "[i] uploading ${CHANGELOG_SRC} as CHANGELOG.md -> generic package registry"
curl --fail --silent --show-error \
     --header "JOB-TOKEN: ${CI_JOB_TOKEN}" \
     --upload-file "${CHANGELOG_SRC}" "${CL_URL}"
ASSET_ARGS+=( --assets-link "{\"name\":\"CHANGELOG.md\",\"url\":\"${CL_URL}\",\"link_type\":\"other\"}" )

# --- 3. GitLab release ---------------------------------------------------
# Idempotent: delete any existing release for this tag before creating.
# Retried pipelines and manual re-runs hit a 409 "Release already exists"
# without this. Packages (step 2) are registry-scoped and unaffected.
RELEASE_API="${CI_API_V4_URL}/projects/${CI_PROJECT_ID}/releases/${TAG}"
if curl --silent --fail --head --header "JOB-TOKEN: ${CI_JOB_TOKEN}" "${RELEASE_API}" >/dev/null 2>&1; then
    echo "[i] release ${TAG} already exists — deleting before recreate"
    curl --silent --fail --request DELETE \
         --header "JOB-TOKEN: ${CI_JOB_TOKEN}" "${RELEASE_API}" || \
        echo "[!] delete failed (may need MAINTAINER token); continuing anyway"
fi

echo "[i] creating GitLab release ${TAG}"
release-cli create \
    --name "v${VERSION}" \
    --tag-name "${TAG}" \
    --description "NOTES.md" \
    "${ASSET_ARGS[@]}"

echo "=== release ${TAG} done ==="
