# CI and release process

How the project's GitLab pipeline builds, tests, packages and publishes
libnvmpi. The pipeline is defined in `.gitlab-ci.yml` and uses the scripts in
`scripts/` (see [build.md](build.md)). The GitHub repository is a push mirror of
the GitLab project; development, CI and releases run on the GitLab instance.

Contents:

- [Pipeline kinds at a glance](#pipeline-kinds-at-a-glance)
- [Stages and jobs](#stages-and-jobs)
- [Runners and tags](#runners-and-tags)
- [Branch and merge request pipelines](#branch-and-merge-request-pipelines)
- [Main-branch pipelines and snapshots](#main-branch-pipelines-and-snapshots)
- [Release pipelines](#release-pipelines)
- [Cutting a release](#cutting-a-release)
- [Published artifacts and layout](#published-artifacts-and-layout)
- [Downstream trigger](#downstream-trigger)
- [Required CI/CD variables](#required-cicd-variables)
- [Changelog generation](#changelog-generation)
- [Reproducing CI steps locally](#reproducing-ci-steps-locally)

## Pipeline kinds at a glance

| Kind | Trigger | Jobs | Publishes |
| --- | --- | --- | --- |
| Branch / merge request | push to a branch, MR event | `build:jp6`, `build:jp5`, `test:api:stub:jp6`, `test:api:stub:jp5`, `test:api:stub:noav1`, `test:api:asan`, `test:api:tsan`, `test:api:hw` (docs-only MR: `ci:docs-only`); plus `dist:*` and `test:deb:*` when a packaging path changed | nothing |
| Main snapshot | push to the default branch | `build:*`, then `dist:jp6`, `dist:jp5`, `test:deb:jp6`, `test:deb:jp5`, `upload:s3:snapshot` (no `test:api:hw`) | tarballs to S3 `snapshots/<short-sha>/` |
| Scheduled / web | schedule, manual web run | full set including `test:api:hw` | snapshot when run on main |
| Release | tag `vX.Y.Z` | the above, then `dist:*`, `test:deb:*`, `release:test:deb:hw`, `upload:s3`, `release:gitlab`, `release:github` | S3 `releases/<ver>/`, package registry, GitLab Release, GitHub Release |
| Downstream | manual | `trigger:jetson-ffmpeg` | starts a jetson-ffmpeg pipeline |

Pipeline de-duplication (`workflow:rules`): tag pipelines always run; a merge
request pipeline runs for MR events; a branch pipeline is suppressed while the
branch has an open MR (only the MR pipeline runs); otherwise branch pipelines
run.

## Pipeline rules (docs-only skip, fast-forward merges, image pins)

- **Docs-only changes do not build.** `changes:` cannot be negated, so the
  code paths (`src include stubs mock cmake vendor scripts packaging test`,
  `CMakeLists.txt`, `nvmpi.pc.in`, `cliff.toml`, `.gitlab-ci.yml`) are listed
  positively. Branch pipelines (main: against the previous push, others:
  against `main`) are created only when one of them changed. Tag, schedule,
  web, API and pipeline-trigger sources always run. An MR always gets a
  pipeline: build and test jobs run only for code changes, otherwise just
  `ci:docs-only`, so the merge gate (pipeline must succeed) still passes. A new
  code directory must be added to the `paths` lists in `.gitlab-ci.yml`.
- **Fast-forward merges.** The project merge method is `ff`, so `main` HEAD is
  the commit the MR pipeline tested. `test:api:hw` (the Orin) therefore does
  not run on main pushes; it runs on MR, tag, schedule and web pipelines.
  `dist:*` need it as `optional`, so tags still gate on it and main snapshots
  do not wait for it.
- **Pinned images.** Builder, runtime and release-tools images come from
  the jetson-docker project with immutable `<tag>-<sha8>` tags
  (`JETSON_DOCKER_SHA`), pulled with `if-not-present`. To adopt new images,
  take the sha8 of the latest green jetson-docker main pipeline and bump it.
- **ccache.** `build:*` use ccache as CMake compiler launcher with a per-job
  GitLab cache (`.ccache`, `CCACHE_COMPILERCHECK=content`).
- **Downstream.** `trigger:jetson-ffmpeg` stays manual until jetson-ffmpeg
  consumes `NVMPI_REF`; then automatic only on tag and nightly pipelines.
- **API test iterations.** `test_api` rapid create/close loops default to 5
  (decoder) and 10 (encoder); set `NVMPI_TEST_ITER` (the weekly schedule does)
  for longer soak runs.

## Stages and jobs

Stages, in order: `build`, `test`, `release-test`, `dist`, `test-deb`, `publish`, `release`, `downstream`.

| Stage | Job | What it does | Runs on |
| --- | --- | --- | --- |
| build | `build:jp6` | Compile libnvmpi against the **stubs** (compile check, not runnable) in the JetPack 6 builder image, run the `bufpool` unit test with `ctest`, install into `_nvmpi/`. | any runner |
| build | `build:jp5` | Same in the JetPack 5 builder image. Catches V4L2 headers/symbols that exist on JetPack 6 (kernel 5.15+) but not on JetPack 5 (kernel 5.10); covers all JetPack 5 releases. | any runner |
| test | `test:api:hw` | Run the `test_api` binary built by `build:jp6` on real Jetson hardware. | Jetson runner |
| test | `test:api:stub:jp6`, `test:api:stub:jp5` | Build the behavioural Jetson mock (`NVMPI_MOCK=ON`) in the JetPack 6 / 5 builder image and run `ctest` (`mock_selftest`, `api_mock`, `api_mock_dec_0..1`, `api_mock_enc_0..2`; `ctest -j`, the decoder suite is split into 2 shards and the encoder suite into 3 with `NVMPI_TEST_SHARD=k/n`). | any runner, no tags |
| test | `test:api:stub:noav1` | Same suite in a plain build with AV1 compiled out (`-DNVMPI_FORCE_NO_AV1=ON`); the builder headers define the AV1 fourcc, so no other job covers that path. | any runner, no tags |
| test | `test:api:asan` | Same suite in an ASan + UBSan + LSan build (`NVMPI_SANITIZER=address`). | any runner, no tags |
| test | `test:api:tsan` | Same suite in a TSan build (`NVMPI_SANITIZER=thread`), run under `setarch -R`. | any runner, no tags |
| dist | `dist:jp6` | Build the **real** (non-stub) library against NVIDIA's apt-repository libraries and package it. Needs `test:api:hw`. | any arm64 runner |
| dist | `dist:jp5` | Same for JetPack 5. Needs `build:jp5` and `test:api:hw`. | any arm64 runner |
| test-deb | `test:deb:jp6` | Install the `dist:jp6` debs in the jetson-docker deb-test image (clean `ubuntu:22.04`, NVIDIA r36.4 apt repo pre-seeded) and run `scripts/test-deb.sh --jp 6 --lintian`. | any runner, no tags |
| test-deb | `test:deb:jp5` | Same in the `ubuntu:20.04` / r35.4 deb-test image, static checks only (see below). | any runner, no tags |
| test-deb | `release:test:deb:hw` | Tags only. Install the `dist:jp6` debs on the Orin, build `test_api` against them via `pkg-config`, check `libnvmpi.so.1` resolves from the system path and run it (`scripts/test-deb-hw.sh`). | Jetson runner |
| publish | `upload:s3` (tags), `upload:s3:snapshot` (main) | Upload the tarballs (and, on tags, the changelog) to the S3 bucket. | release-tools image |
| release | `release:gitlab` | Upload to the GitLab generic package registry and create the GitLab Release. Tags only. | release-tools image |
| downstream | `trigger:jetson-ffmpeg` | Manual: start a jetson-ffmpeg pipeline for this commit. | trigger |

Details per job:

- **`build:*`** run `scripts/build.sh --stubs --clean --prefix "$NVMPI_PREFIX"`
  then `ctest -R bufpool` and `cmake --install build`. Artifacts: `_nvmpi/` and
  `build/`, kept 1 hour. `build/` carries the stub-linked `test_api` used by
  the Jetson test.
- **`test:api:hw`** is described in [Hardware test](#hardware-test).
- **`dist:*`** install NVIDIA's `nvidia-l4t-multimedia` and
  `nvidia-l4t-multimedia-utils` packages from NVIDIA's public Jetson apt
  repository inside a plain container (a touch file skips the package's "must run
  on a Tegra platform" pre-install check), then run
  `scripts/build.sh --no-stubs --clean --install` and `scripts/package.sh`.
  `--no-stubs` hard-fails if the real libraries are missing, so a release can
  never ship a stub-linked tarball. Artifacts: `dist/`, kept 1 week.
- **`test:deb:*`** run `scripts/test-deb.sh` in the jetson-docker deb-test image
  (`DEB_TEST_IMAGE_JP6`/`_JP5`): plain ubuntu with the apt, CA and tool setup
  pre-seeded and the runtime deps downloaded but not installed. It is not the
  builder image (which already carries every library), so missing package
  dependencies show up. Steps: install `libnvmpi1` alone (dependency
  resolution); `ldconfig` lists `libnvmpi.so.1` and the unversioned
  `libnvmpi.so` is absent; JP6 `dlopen("libnvmpi.so.1")` + `dlsym` of the codec
  entry points (the jetson-ffmpeg scenario), JP5 a static `readelf` export
  check because NVIDIA's r35.4 libs abort in a constructor without
  `/dev/nvmap` (so JP5 also only compiles and links the `libnvmpi-dev`
  program, JP6 runs it); install `libnvmpi-dev` (`libnvmpi.so` symlink,
  `nvmpi.h`, `nvmpi.pc`, pkg-config compile and link); purge both and check no
  owned file remains; lintian (`E:` tags fail, others are printed). Run it
  locally with `scripts/test-deb.sh --jp 6 --debdir <dir>` inside a container.
  `dist:*` and `test:deb:*` run on tags, main and MRs that change
  `scripts/package*.sh`, `scripts/test-deb.sh`, `packaging/`, `CMakeLists.txt`,
  `nvmpi.pc.in` or `.gitlab-ci.yml`. `upload:s3`, `release:gitlab` and
  `release:github` need both jobs, so a broken deb blocks the release.
- **`release:test:deb:hw`** (tags only) extends `test:api:hw` (image, runner
  tags, `resource_group`, env) and runs `scripts/test-deb-hw.sh --debdir dist`:
  install `libnvmpi1` + `libnvmpi-dev` from the `dist:jp6` artifacts, compile
  `test/api/test_api.c` with `pkg-config --cflags --libs nvmpi` only (no build
  tree), assert via `ldd` that `libnvmpi.so.1` resolves from the system lib
  path, run `test_api` on the hardware at the default iteration depth, purge.
  Installs `gcc`/`pkg-config` first if the image lacks them. `upload:s3`,
  `release:gitlab` and `release:github` need it on tags (non-optional); on main
  `upload:s3:snapshot` has no such need because the job does not exist there.
- **`upload:s3`** (tags only) runs with full git history (`GIT_DEPTH: 0`) and
  passes `NOTES.md` on as an artifact so the changelog is generated only once.
  **`upload:s3:snapshot`** (default branch) shares the hidden `.upload-s3`
  base, uploads to `snapshots/<short-sha>/` and has no artifacts.
- **`release:gitlab`** runs `scripts/release.sh`.

### Stub, sanitizer and hardware jobs

The mock jobs are **additive**: no hardware suite is removed or replaced, and
`test:api:hw` runs exactly what it ran before. The mock catches logic,
lifecycle, memory and thread bugs early; real driver behaviour is only proven
on the Orin.

- All of `test:api:stub:*`, `test:api:asan`, `test:api:tsan` and `test:api:hw`
  start in parallel: the mock jobs use `needs: []` (they build their own tree),
  `test:api:hw` needs `build:jp6`.
- `workflow:auto_cancel:on_job_failure: all` cancels the rest of the pipeline,
  including a still-queued `test:api:hw`, when any job fails, so a mock failure
  does not occupy the Jetson runner.
- Forked test children end with `_exit()`, which skips LeakSanitizer's exit
  check. `test/api/leak_check.h` runs `__lsan_do_leak_check()` before each
  `_exit()` in ASan builds (no-op otherwise), so leaks fail the offending case
  without valgrind.
- GCC 11's libtsan does not intercept `pthread_cond_clockwait`, which makes
  `condition_variable::wait_for` look like a double lock / data race on every
  `NVMPI_bufPool` queue. TSan builds with GCC < 12 therefore force-include
  `cmake/tsan_compat.h`, which switches libstdc++ back to the intercepted
  `pthread_cond_timedwait` (test builds only).
- `test/tsan.supp` holds the one remaining suppression: benign plain-bool flag
  reads in NVIDIA's vendored `NvV4l2ElementPlane` DQ thread.
- Sanitizer reports fail the job even if ctest exits 0: the mock jobs capture
  the ctest output to `ctest-output.log` and run `scripts/check-test-log.sh`,
  which fails on `ERROR: AddressSanitizer`, `ERROR: LeakSanitizer`,
  `runtime error:` (UBSan), `WARNING: ThreadSanitizer` or `SUMMARY: ...Sanitizer`.
  Reports silenced by `test/tsan.supp` are never printed. Run it locally on any
  log: `scripts/check-test-log.sh <log>...` (exit 1 on a report, 2 on an
  unreadable log). The log is kept as an artifact on failure.
- Locally: `cmake -S . -B build-asan -DNVMPI_MOCK=ON -DNVMPI_SANITIZER=address`
  (or `thread`), build, `scripts/mock-dev-nodes.sh`, `ctest`.

### Hardware test

`test:api:hw` runs `build/test_api` (see
[Testing](../README.md#testing)) with:

```text
NVMPI_TEST_STRICT=1      # crashes fail the job
NVMPI_TEST_VERBOSE=1     # keep child stderr so abort and driver messages show
NVIDIA_VISIBLE_DEVICES=all
NVIDIA_DRIVER_CAPABILITIES=all
```

The binary is stub-linked, but the stubs carry the real sonames, so the NVIDIA
container runtime's injected tegra libraries satisfy them at load time. The job
puts the real library directories (`.../nvidia`, `.../tegra`) first in
`LD_LIBRARY_PATH`, because the build tree's stub directory is in the binary's
RUNPATH; if the binary instead has a `DT_RPATH` (which would beat
`LD_LIBRARY_PATH`), the job preloads the real libraries. It prints read-only
diagnostics first (injected libraries, device nodes, the binary's RPATH/RUNPATH
and `ldd`).

Design constraints:

- **Read-only toward the host.** The job installs nothing and rebuilds nothing:
  the runtime bind-mounts the host's tegra libraries into the container, so
  `apt` would try to overwrite them and fail. It only reads the host libraries
  and device nodes.
- **Serialised.** `resource_group: jetson` ensures one Jetson job at a time. The
  Tegra V4L2 driver segfaults on concurrent engine access instead of returning
  `EBUSY`, so hardware jobs must never overlap with other hardware work on the
  same device (see [internals.md](internals.md#tegra-v4l2-driver-pitfalls)).
- Timeout 10 minutes.
- Only JetPack 6 is hardware tested. The JetPack 5 build is compile-checked, and
  the JetPack 5 tarball is gated on the JetPack 6 hardware result.

## Runners and tags

| Tag | Used by | Purpose |
| --- | --- | --- |
| _(none)_ | `build:*`, `dist:*`, `upload:s3`, `release:*`, `trigger:*` | Run on any available runner; the project's runners are Kubernetes (k3s) based. No GPU needed. |
| `jetson` and `orin-nx` | `test:api:hw` only | Selects the runner with real Orin NX hardware. Both tags are required. |

Adding a new hardware variant means registering a project runner tagged
`jetson` plus the variant tag and adding a job (or matrix entry) that selects
it. If a Jetson job reports no matching runner, check that a runner carrying
both tags is online.

Builder and runtime images (JetPack 5 and JetPack 6 builder, the Jetson test
image and the release-tools image) are built and published by the
jetson-docker project's pipeline; this project only consumes them.

## Branch and merge request pipelines

`build:jp6`, `build:jp5` and `test:api:hw`. These jobs carry no `rules:` of
their own: `workflow:rules` already limits pipelines, and they must also run on
tag pipelines, so a release only happens after the Jetson test passed on that
exact commit. Nothing is published.

## Main-branch pipelines and snapshots

A pipeline on the default branch (and not an MR pipeline) runs build and test
and then `dist:jp6`, `dist:jp5` and `upload:s3:snapshot`. There is no release gate:

- The package version is `<CMake VERSION>-<short-sha>`, for example
  `3.10.0-1b009ec` (the plain short SHA, no `g` prefix).
  The `-<short-sha>` build metadata marks the archive as a snapshot.
- `upload:s3:snapshot` uploads to `snapshots/<short-sha>/jp5|jp6/` (no changelog).
- Snapshots are not GitLab Releases and do not touch the package registry.

Scheduled pipelines on the default branch follow the same path, so a pipeline
schedule (configured in the project settings, not in `.gitlab-ci.yml`) produces
periodic snapshots. The project runs this weekly.

## Release pipelines

A release is a tag matching `^v[0-9]+\.[0-9]+\.[0-9]+$`. The tag pipeline runs
build and test too, so a release is gated on the Jetson test of that commit.

1. `dist:jp6` / `dist:jp5` check **the tag must equal the CMake `VERSION`**
   (`v3.10.0` requires `project(nvmpi VERSION 3.10.0 ...)`), then build and
   package. A mismatch fails the job.
2. `upload:s3` runs `scripts/changelog.sh` (git-cliff, writes `NOTES.md`) and
   `scripts/upload-s3.sh "releases/<ver>" NOTES.md`.
3. `release:gitlab` runs `scripts/release.sh`: uploads each tarball to the GitLab
   generic package registry as package `jetson-nvmpi`, version `<ver>`, then
   creates the GitLab Release `v<ver>` with `NOTES.md` as description and the
   tarballs as `package` asset links. It is idempotent: an existing release for
   the tag is deleted first (retried pipelines would otherwise hit "release
   already exists"); the registry upload is unaffected.

4. `release:github` runs `scripts/release-github.sh` (independent of
   `release:gitlab`): waits for the push mirror to deliver the tag to GitHub,
   checks the tag SHA, then creates (or updates) the GitHub Release `v<ver>`
   with `NOTES.md` as body and uploads the tarballs, debs and `CHANGELOG.md`
   as assets. Needs the masked CI/CD variable `GITHUB_RELEASE_TOKEN`
   (contents: write on the GitHub repo). Re-runs replace same-named assets.

## Cutting a release

In Claude Code, run `/create-release [vX.Y.Z]`
(`.claude/skills/create-release/SKILL.md`); it does the steps below. By hand:

```bash
# 1. bump the version in CMakeLists.txt: project(nvmpi VERSION X.Y.Z ...)
# 2. regenerate CHANGELOG.md (only post-split history, v3.10.0..HEAD)
git-cliff --config cliff.toml --tag vX.Y.Z v3.10.0..HEAD -o CHANGELOG.md
# 3. commit both on the default branch, push
# 4. tag that commit; the tag must be v<that exact version>
git tag -a vX.Y.Z -m "vX.Y.Z"
git push origin vX.Y.Z
```

`CHANGELOG.md` and the release notes come from the same `cliff.toml` and the
Conventional Commit history; nothing is written by hand. Follow
[SemVer](https://semver.org/). If a tag pipeline fails, fix the cause, delete the
partial tag/release and re-tag; do not continue with a red release pipeline.

## Published artifacts and layout

Archive name: `jetson-nvmpi_<ver>_l4t<tag>_aarch64.tar.gz`, where `<tag>` is the
L4T release, currently `r36.4.0` for JetPack 6 and `r35.4.1` for JetPack 5,
giving names like `jetson-nvmpi_3.10.0_l4tr36.4.0_aarch64.tar.gz`. Contents and
installation are in [build.md](build.md#install-a-release-package).

S3 layout (one folder per JetPack, derived from the L4T major in the file name:
`l4tr36.*` to `jp6`, `l4tr35.*` to `jp5`; any other name fails the upload):

```text
releases/<ver>/jp5/<archive>
releases/<ver>/jp6/<archive>
releases/<ver>/CHANGELOG.md
snapshots/<short-sha>/jp5/<archive>
snapshots/<short-sha>/jp6/<archive>
```

Uploads are plain `curl` SigV4 PUTs (`--aws-sigv4`, needs curl 7.75 or newer; no
`mc` client) followed by a signed HEAD that verifies the object size. Object keys
are restricted to a safe character set. Credentials are only handed to `curl`.

GitLab side, on tags only:

- Generic package registry: `jetson-nvmpi/<ver>/<archive>`.
- GitLab Release `v<ver>` with the changelog and the package asset links.

Job artifacts: `build/` and `_nvmpi/` (1 hour), `dist/` (1 week), `NOTES.md`
(1 week).

## Downstream trigger

`trigger:jetson-ffmpeg` starts the [jetson-ffmpeg](https://github.com/gjrtimmer/jetson-ffmpeg)
pipeline against this commit. It is **manual** (and `allow_failure`) on tags, on
MR pipelines and on the default branch: jetson-ffmpeg does not consume the
reference yet, so an automatic trigger would burn a full Jetson hardware run for
nothing. Variables passed:

| Variable | Value | Purpose |
| --- | --- | --- |
| `NVMPI_REF` | the ref name (branch or tag) | jetson-ffmpeg's job token can fetch `dist:*` artifacts by ref name, not by SHA |
| `NVMPI_SHA` | the full commit SHA | identifies the exact commit (`snapshots/<short-sha>/` on S3) |

## Required CI/CD variables

Names only; set them in the project's CI/CD settings. A missing required value
is a hard failure.

| Variable | Used by | Notes |
| --- | --- | --- |
| `S3_HOST` | `scripts/upload-s3.sh` | Base URL of the S3 endpoint. |
| `S3_BUCKET` | `scripts/upload-s3.sh` | Bucket name. |
| `S3_ACCESS_KEY` | `scripts/upload-s3.sh` | Credential; mask it. |
| `S3_SECRET_KEY` | `scripts/upload-s3.sh` | Credential; mask it. |
| `S3_REGION` | `scripts/upload-s3.sh` | Optional, default `us-east-1`. |
| `DOCKERHUB_USERNAME` | `.gitlab-ci.yml` | Used to compose the release-tools image reference. |

Provided by GitLab CI itself and used by the scripts: `CI_COMMIT_TAG`,
`CI_COMMIT_SHORT_SHA`, `CI_COMMIT_SHA`, `CI_COMMIT_REF_NAME`, `CI_API_V4_URL`,
`CI_PROJECT_ID`, `CI_JOB_TOKEN`. No GitHub token is needed: the GitHub
repository is updated by the push mirror.

Plain pipeline variables (not secrets) defined in `.gitlab-ci.yml`: `L4T_TAG`
(`r36.4.0`), `L4T_SOC` (`t234`), `L4T_TAG_JP5` (`r35.4.1`), `NVMPI_PREFIX`. The
JetPack 5 job overrides `L4T_TAG` and sets `L4T_SOC` to `t194`.

## Changelog generation

`scripts/changelog.sh` runs `git-cliff --config cliff.toml --latest --strip header -o NOTES.md`
for the tag (it needs full history and tags). If git-cliff fails it falls back to
a plain `git log` list between the previous tag and the current one. `cliff.toml`
keys on Conventional Commits:

| Commit type | Changelog section |
| --- | --- |
| `feat` | Features |
| `fix` | Bug Fixes |
| `perf` | Performance |
| `refactor` | Refactor |
| `build` | Build |
| `test` | Testing |
| body mentions `security` | Security |
| `Revert` | Reverts |

`chore`, `style`, `ci`, `docs` and merge commits are skipped, as is anything
unconventional. This is why commit messages must follow
[Conventional Commits](https://www.conventionalcommits.org/).

## Reproducing CI steps locally

```bash
# build job (any host with the Multimedia API headers)
scripts/build.sh --stubs --clean --prefix "$PWD/_nvmpi"
(cd build && ctest -R bufpool --output-on-failure)

# dist job (on a Jetson, or in a container with the real libraries)
scripts/build.sh --no-stubs --clean --install --prefix "$PWD/_pkg"
scripts/package.sh --prefix "$PWD/_pkg" --version 3.10.0-local --l4t r36.4.0

# hardware test (on a Jetson, with nothing else using NVDEC/NVENC)
NVMPI_TEST_STRICT=1 NVMPI_TEST_VERBOSE=1 ./build/test_api
```

`scripts/release.sh` needs the GitLab CI environment and a release-tools image
(`release-cli`, `git-cliff`) and is exercised by the real tag pipeline.
