# Detect My Jiggler (vendored)

The detection algorithms from https://github.com/detectmyjiggler/detect_my_jiggler
at commit `b638ce84c14ef524816c8bb009de7723b7afcd63`, MIT licensed (see `LICENSE`).

These files are copied unmodified:

- `detection.cpp`, `detection.h`: the detection algorithms
- `detection_constants.h`: thresholds
- `device_ids.h`: VID allow/block lists and trust scoring
- `mouse_device.h`: per-device state

The Windows GUI, Raw Input handling and detection loop (`main.cpp`) aren't
copied. The parts of `main.cpp` that feed movement into `MouseDevice` and
decide when a device counts as a jiggler are ported in `../../src/detector.cpp`.
`ExtractVidPid` from `device_utils.cpp` is copied in `../../src/extract_vid_pid.cpp`.
To update, copy the files from a newer commit and check whether those ported
parts of `main.cpp` changed.
