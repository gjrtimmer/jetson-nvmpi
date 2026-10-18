# Changelog

libnvmpi was split out of jetson-ffmpeg after v3.10.0. History up to and
including v3.10.0 is in the jetson-ffmpeg CHANGELOG.

## 4.0.0 - 2026-10-18

### Bug Fixes


**ci**


- Upload to S3 with curl sigv4, drop mc
- Avoid SIGPIPE false fail in deb checks
- Avoid SIGPIPE false fail in curl sigv4 check


**nvmpi**


- Stop encoder DQ thread safely on close without frames
- Send EOS before stopping encoder DQ thread on close


### Build


**cmake**


- Split deb into libnvmpi1 and libnvmpi-dev, bump to 4.0.0


### Features


**mock**


- Add behavioural V4L2 and NvBufSurface mock libs


### Testing


**api**


- Add strict and verbose modes


**mock**


- Add mock self-test and wire ctest entries

