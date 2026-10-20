# Changelog

libnvmpi was split out of jetson-ffmpeg after v3.10.0. History up to and
including v3.10.0 is in the jetson-ffmpeg CHANGELOG.

## 4.3.0 - 2026-10-20

### Features


**dec**


- Nvmpi_decoder_get_frame_fd2 with per-plane geometry (#138, #129)


## 4.2.2 - 2026-10-20

### Bug Fixes


**dec**


- Refcounted frame-pool generations; rebuild on resolution change (#137)


### Testing


**api**


- Diagnose decoder fd counts against a warmed-up baseline (#137)


## 4.2.1 - 2026-10-20

### Bug Fixes


**jpeg**


- Crop, not scale, the padded decode in fd mode (#135)


### Testing


**mock**


- NvJPEGDecoder model and JPEG fd mock suite (#128)
- Drop disableMjpegDecode from the JPEG decoder mock (#128)


## 4.2.0 - 2026-10-20

### Bug Fixes


**enc**


- Reject unsupported codecs before opening the encoder (#134)


### Features


**dec**


- AV1 hardware decode (#7, #61)


### Testing


**api**


- Shard the decoder mock suite in two (#130)
- Use a 320x240 AV1 fixture (NVDEC rejects 64x64) (#7)


## 4.1.0 - 2026-10-20

### Bug Fixes


**jpeg**


- Read the SOF component count at the right offset (#4)
- Report the displayed JPEG size in nvmpi_fd_frame (#4)


### Features


**jpeg**


- DMA-BUF fd output for the JPEG decoder (#4)


### Testing


**api**


- Widen the no-data decoder close bound to 400 ms (#132)
- Make mock latency and fault-control checks load-tolerant (#133)


## 4.0.9 - 2026-10-20

### Bug Fixes


**enc**


- Log a pool-empty drop during teardown drain at debug (#131)


### Performance


**enc**


- Return at once from get_packet(wait) when nothing is in flight (#103)


## 4.0.8 - 2026-10-20

### Bug Fixes


**enc**


- No error log when close/flush stops the capture DQ thread (#122)


**surface**


- Log missing DMA_BUF sync at debug level (#122)


### CI


**test**


- Run the mock ctests in parallel (#123)


### Documentation


**ci**


- Describe the sharded mock ctests (#130)


### Performance


**dec**


- Close a never-fed decoder in ~50 ms instead of ~1 s (#105)


### Refactor


**dec**


- Move close/flush and the frame pool into own files (#105)


### Testing


**api**


- Run encoder mock cases strict (#122)
- Shard the encoder mock suite (#130)


## 4.0.7 - 2026-10-20

### Bug Fixes


**enc**


- Keep the in-flight packet visible to the drain before close (#121)
- Back-pressure nonblocking put_frame instead of dropping AUs (#125)


### CI


**mock**


- Check every mock binary for RPATH, fail on missing (#119)
- Gate mock jobs on sanitizer reports in ctest output


**scripts**


- Add check-test-log.sh sanitizer report scanner


**upload**


- Split upload:s3 into tag and snapshot jobs (#119)


### Refactor


**enc**


- Split the DMA-BUF put_frame_fd path out of nvmpi_enc_input.cpp (#125)


### Testing


**api**


- Capture child stderr, mark expected fault errors (#120)


**mock**


- Decodable H.264 slices (I_PCM + P_Skip) in the encoder (#117)
- Size enc test packets to NVMPI_ENC_CHUNK_SIZE (#117)
- Decodable HEVC slices (CABAC PCM + skip) in the encoder (#118, #110)


## 4.0.6 - 2026-10-20

### Testing


**mock**


- Heap AU buffer, frame rate and VUI timing in the encoder (#116)


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

