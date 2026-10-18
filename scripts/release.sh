#!/usr/bin/env bash
# Create the release: changelog -> GitLab generic package registry -> GitLab
# Release. Intended to run inside the release-tools image on a tag pipeline
# (see .gitlab-ci.yml "release" job). Not meant for local use, but every
# external dependency is an env var so it can be dry-run.
#
# Required (provided by GitLab CI on a tag pipeline):
#   CI_COMMIT_TAG, CI_API_V4_URL, CI_PROJECT_ID, CI_JOB_TOKEN
# Inputs:
#   dist/*.tar.gz - archives produced by scripts/package.sh
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
cd "${REPO_ROOT}"

TAG="${CI_COMMIT_TAG:?CI_COMMIT_TAG is required (run on a tag pipeline)}"
VERSION="${TAG#v}"
DIST="${REPO_ROOT}/dist"

echo "=== releasing ${TAG} (version ${VERSION}) ==="

# --- 1. changelog --------------------------------------------------------
# git-cliff needs full history + tags; the CI job fetches them (GIT_DEPTH: 0).
echo "[i] generating changelog with git-cliff"
git-cliff --config cliff.toml --latest --strip header -o NOTES.md || {
    echo "[!] git-cliff failed; falling back to raw git log"
    PREV_TAG="$(git describe --tags --abbrev=0 "${TAG}^" 2>/dev/null || true)"
    { echo "## ${VERSION}"; echo;
      if [ -n "${PREV_TAG}" ]; then git log --pretty='- %s' "${PREV_TAG}..${TAG}";
      else git log --pretty='- %s' "${TAG}"; fi; } > NOTES.md
}
echo "----- NOTES.md -----"; cat NOTES.md; echo "--------------------"

# --- 2. upload archives to GitLab generic package registry ---------------
shopt -s nullglob
ARCHIVES=( "${DIST}"/*.tar.gz )
if [ ${#ARCHIVES[@]} -eq 0 ]; then echo "[E] no archives in ${DIST}" >&2; exit 1; fi

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
