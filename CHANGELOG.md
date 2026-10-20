# Changelog

libnvmpi was split out of jetson-ffmpeg after v3.10.0. History up to and
including v3.10.0 is in the jetson-ffmpeg CHANGELOG.

## 4.0.5 - 2026-10-20

### Testing


**mock**


- Emit real HEVC VPS/SPS/PPS from the encoder model (#108)


## 4.0.4 - 2026-10-20

### CI


**release**


- Run test_api on the Orin against the installed debs (#115)


## 4.0.3 - 2026-10-20

### Testing


**mock**


- Convert NV12 <-> I420 in NvBufSurfTransform (#109)


## 4.0.2 - 2026-10-19

### Bug Fixes


**enc**


- Do not lose tail packets when draining at EOS (#111)


### Documentation


**api**


- Document all return codes of create/close/get_frame (#95)


### Testing


**mock**


- Add encoder determinism and tail-loss stress case

## 4.0.1 - 2026-10-19

### Bug Fixes


**ci**


- Allow + in S3 object keys for snapshot debs


**deb**


- Depend on libjpeg-turbo8, ship shlibs and changelog, strip library


### Testing


**deb**


- Add clean-system install test script for libnvmpi debs
- Show apt output of the base tool install
- Skip ca-certificates when the runner mounts a CA bundle
- Print CA diagnostics when the NVIDIA key fetch fails
- Use a private CA bundle instead of installing ca-certificates
- Accept snapshot deb versions in the nvmpi.pc version check

## 4.0.0 - 2026-10-19

### Bug Fixes


**ci**


- Upload to S3 with curl sigv4, drop mc
- Avoid SIGPIPE false fail in deb checks
- Avoid SIGPIPE false fail in curl sigv4 check


**dec**


- Survive NvBufSurface allocation failure
- Build nvmpi_alloc_surfaces on JetPack 5 headers


**enc**


- Survive NvBufSurface allocation failure


**mock**


- Register EXPBUF fds as NvBufSurface


**nvmpi**


- Stop encoder DQ thread safely on close without frames
- Send EOS before stopping encoder DQ thread on close
- Give decoder and encoder distinct context types


**test**


- Use the real LSan leak-check symbol


### Build


**cmake**


- Split deb into libnvmpi1 and libnvmpi-dev, bump to 4.0.0
- Route TSan builds around GCC 11 clockwait gap


### Features


**cmake**


- Add NVMPI_SANITIZER for mock builds


**mock**


- Add behavioural V4L2 and NvBufSurface mock libs
- Add decoder model and fault/delay injection
- Add encoder model


### Testing


**api**


- Add strict and verbose modes
- Trim rapid lifecycle loops, add NVMPI_TEST_ITER


**mock**


- Add mock self-test and wire ctest entries
- Cover decoder model, fault injection and libnvmpi decode
- Cover the encoder model and libnvmpi encode

