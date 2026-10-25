# Changelog

libnvmpi was split out of jetson-ffmpeg after v3.10.0. History up to and
including v3.10.0 is in the jetson-ffmpeg CHANGELOG.

## 4.9.0 - 2026-10-25

### Bug Fixes


**jpeg**


- New NvJPEGEncoder when the encoded size changes


**vic**


- Copy at scale 1:1 with the Nearest filter


### Features


**jpeg**


- Add nvmpi_jpeg_encoder_put_frame_fd


### Refactor


**jpeg**


- Split JPEG encoder buffer side from the API file


### Testing


**jpeg**


- Cover nvmpi_jpeg_encoder_put_frame_fd
- Print put_frame_fd MSEs and surface format on stderr
- Print plane samples of the fd and CPU results
- Print surface readback and chroma edge samples
- Print chroma band errors
- Judge the yuv420 fd case against the surface pixels
- Say why a decode in the fd encoder cases failed
- Print the SOF size of an undecodable JPEG
- Use 648x360 for the padded-pitch NV12 fd case
- Use 656x360 for the padded-pitch NV12 fd case
- Fix the duplicated block in the 656x360 case change
- Back to 642x358 for the padded-pitch NV12 fd case

### Documentation


**api**


- Document nvmpi_jpeg_encoder_put_frame_fd

## 4.8.0 - 2026-10-25

### Features


**api**


- Add nvmpi_surface_get_layout


### Testing


**api**


- Surface layout cases

### Documentation


**api**


- Document nvmpi_surface_get_layout


**general**


- Remove internal references from public docs

## 4.7.1 - 2026-10-25

### Bug Fixes


**decoder**


- Re-arm the capture plane on flush so a resume needs no event


### Testing


**api**


- Cover flush after the first event with the mock decoder
- Add hardware case for flush after the first event


**mock**


- Make decoder re-announce after OUTPUT flush switchable

## 4.7.0 - 2026-10-24

### Features


**api**


- Add nvmpi_get_caps capability bits


### Testing


**api**


- Nvmpi_get_caps cases

### Documentation


**api**


- Document nvmpi_get_caps

## 4.6.6 - 2026-10-24

### Bug Fixes


**dec**


- Drain capture plane to LAST after input EOS


**decoder**


- Let close() interrupt the race-guard event wait


### Testing


**api**


- Assert full frame count at decoder EOS
- Bound close() latency inside the race guard


**mock**


- Model decoder reorder delay (NVMPI_MOCK_DEC_REORDER)

## 4.6.5 - 2026-10-23

### Bug Fixes


**enc**


- Odd frame sizes through put_frame_fd (use_dmabuf): the encoder kept the caller's odd w x h, so the Tegra encoder replaced the last chroma column/row with its own values. On NvUtils builds the DMA-BUF encoder is now configured at the even-rounded size, like put_frame since 4.6.3. The staging copy reads the caller's surface at its full geometry (chroma ceil(w/2) x ceil(h/2)) and replicates the last column/row into the padding. An fd that is not an NvBufSurface (dup'd or foreign) cannot be padded: at an odd size put_frame_fd refuses it with NVMPI_ERR_INVAL before taking an OUTPUT slot. Legacy nvbuf_utils builds refuse use_dmabuf with an odd width or height at create. API unchanged (#156)


**jpeg**


- Drop nested /* in SOF comment (-Wcomment)


### Build


**cmake**


- Non-/usr prefix note as STATUS, not WARNING


### CI


- Bump JETSON_DOCKER_SHA to 43214410: the deb-test image preinstalls the dependencies of nvidia-l4t-multimedia and libjpeg-turbo8, so test:deb runs faster
- The release hw deb test builds test_api with the Jetson Multimedia API headers and libnvbufsurface, with NVMPI_REQUIRE_SURF: a missing nvbufsurface.h is now a build error instead of a silently skipped or falsely failed NvBufSurface case (#156)


### Refactor


**enc**


- Share the copy-and-pad row loop between put_frame and put_frame_fd


### Testing


**api**


- Odd-size put_frame_fd mock cases (padded copy, and an unregistered fd refused) and an Orin round trip that checks the last chroma column/row arrive (#156)


## 4.6.4 - 2026-10-22

### Testing


**api**


- Sanitizer builds give the mock tests headroom: the decoder mock cases run in 4 shards instead of 2, and NVMPI_MOCK_TEST_TIMEOUT is 600 s for sanitizer builds. On CI the ASan decoder shards dropped from ~155 s to ~90 s each, against a 600 s limit (was 300 s, and a loaded runner timed them out) (#153)


## 4.6.3 - 2026-10-22

### Bug Fixes


**enc**


- Odd frame sizes: put_frame copied chroma at the floored NvBuffer geometry, so the last chroma column/row never reached the encoder. With the plane at 641x357 the Tegra encoder also replicated the interior into them. For 4:2:0 input with an odd width or height, the encoder is now configured at the even-rounded size (641x357 is coded as 642x358, as any 4:2:0 encoder must). libnvmpi copies luma w x h with the last column/row replicated into the padding, and chroma at the full (ceil) geometry. Callers whose payload_size covers only the floored chroma keep the floored copy, with the padding replicated. API unchanged; put_frame_fd (caller DMA-BUF) unchanged (#154)


### Testing


**api**


- Odd-size chroma mock cases (the mock models the encoder's edge replication) and an Orin round-trip probe that checks the last chroma column/row arrive (#154)


## 4.6.2 - 2026-10-22

### Bug Fixes


**jpeg**


- nvmpi_jpeg_encoder_put_frame refuses a linesize below the row bytes, or a non-zero payload_size[p] smaller than the bytes it reads, with NVMPI_ERR_INVAL before anything is copied or encoded. Before, a frame smaller than the configured size was read past the caller's buffer. payload_size 0 stays accepted (#151)


### Testing


**api**


- JPEG encoder plane size cases, with a mock NvJPEGEncoder for mock builds (#151)


## 4.6.1 - 2026-10-22

### Bug Fixes


**enc**


- nvmpi_encoder_put_frame checks each caller plane whose payload_size[p] is set against the bytes it reads ((rows-1)*linesize + row bytes) and returns NVMPI_ERR_INVAL before taking an OUTPUT buffer. Before, a frame smaller than the configured size was read past the caller's buffer. payload_size 0 stays accepted (#150)


### CI


- Use the jetson-docker images and a prebaked deb-test image for test:deb (#152)


### Testing


**api**


- put_frame plane size cases (#150)


## 4.6.0 - 2026-10-22

### Bug Fixes


**dec**


- get_frame checks the caller's plane layout against the decoded frame. A frame that does not fit (linesize shorter than a row, or larger than the caller's capacity or the size it allocated for) is refused with NVMPI_ERR_FRAME_SIZE instead of being copied. Before, a mid-stream resolution increase wrote past the caller's buffers (#149)


**jpeg**


- nvmpi_jpeg_decoder_get_frame bounds its copy by the caller's buffers in the same way (#149)


### Features


**dec**


- New nvmpi_decoder_get_frame2: the caller passes per-plane capacity in nvFrame.payload_size[]. It reports the decoded width/height and minimum plane sizes, and keeps a frame that does not fit pending so the caller can reallocate and retry. New error code NVMPI_ERR_FRAME_SIZE (-90). nvFrame layout and soname unchanged (#149)


### Testing


**api**


- get_frame / get_frame2 / JPEG plane size cases, including a resolution increase against exact-size caller buffers (#149)


## 4.5.3 - 2026-10-22

### Bug Fixes


**av1**


- Reject P010 input for AV1 encode: the Orin's (L4T R36.4) AV1 encoder cannot take 10-bit input, and the VIC conversion fails with BlockSide error 0x4 (#146)


**dec**


- Drop AV1 input until a sequence header has been seen; NVDEC held those OUTPUT buffers and put_packet blocked forever (#147)


**dec,enc**


- Make the pts round trip total for any int64, including AV_NOPTS_VALUE and negatives (#148)


### Testing


**api**


- AV1 P010 refused, AV1 decode without an in-band sequence header (#146, #147)
- Pts round trip cases for decoder and encoder (#148)


**mock**


- Model the AV1 decoder before the first sequence header like the Orin (#147)
- Store V4L2 buffer timestamps as u64 ns like the kernel (#148)


## 4.5.2 - 2026-10-21

### Bug Fixes


**enc**


- Refuse frames the packet pool cannot hold in blocking mode: put_frame returns NVMPI_ERR_EAGAIN instead of dropping the AU later (#145)


### Testing


**enc**


- Wrapper-shaped encode/EOS drain regression case
- Reader lagging past the packet pool in blocking mode (#145)


### Scripts

- Add scripts/prune-pipelines.sh (delete failed, skipped and warning pipelines)


## 4.5.1 - 2026-10-21

### Bug Fixes


**dec**


- Service resolution change while the frame pool is full (#143)


### Testing


**dec**


- Resolution change with a full frame pool (#143)


## 4.5.0 - 2026-10-21

### Features


**encoder**


- Accept P010 input and HEVC Main10 profile; map HEVC profiles to the H.265 enum (#126)


### Testing


**api**


- Cover P010 input and HEVC Main10 encode (#126)
- Check 8-bit HEVC programs and declares Main (#144)


**mock**


- Model HEVC Main10 in the encoder


## 4.4.1 - 2026-10-20

### Testing


**api**


- Scale enc determinism runs down under sanitizers (#142)


## 4.4.0 - 2026-10-20

### Bug Fixes


**enc**


- Strip IVF framing from AV1 packets (#8)


### Features


**enc**


- AV1 hardware encode (#8)


**mock**


- AV1 encoder model (#8)


### Testing


**api**


- AV1 encode cases (#8)
- Accept split AV1 frame OBUs, dump bad packets


**mock**


- Emit driver IVF framing for AV1


## 4.3.2 - 2026-10-20

### Testing


**api**


- Dump all thread stacks when a mock case times out (#136)


## 4.3.1 - 2026-10-20

### Bug Fixes


**enc**


- Honour nvEncParam.peak_bitrate in VBR mode (#141)


### Refactor


**enc**


- Split encoder parameter translation out of nvmpi_enc_init.cpp (#140)


### Testing


**api**


- Check the VBR peak bitrate the encoder programs (#141)
- Read mock peak bitrate via weak symbol


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

