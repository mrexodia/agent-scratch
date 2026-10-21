# Jiggler detection test

Checks whether the wiggler's movement can be told apart from a real mouse.
It feeds the wiggler's movement generators
(`../rp2040-one-mouse-wiggler/lib/wiggle`, the same code the firmware runs)
through the detection algorithms of
[Detect My Jiggler](https://github.com/detectmyjiggler/detect_my_jiggler).

Everything runs in simulated time on the host, so 30 minutes of wiggling
takes well under a second, and no hardware or Windows is needed.

## What is simulated

- **Detector:** the algorithms from `third_party/detect_my_jiggler` are used
  unmodified (MIT, see `UPSTREAM.md`). The parts of the app's `main.cpp` that
  record each mouse report and run the detection pass every 200 ms are ported
  to `src/detector.cpp` with the same thresholds. The device is seen with the
  VID/PID 046D:C077 that the wiggler reports, so it gets the "Verified
  Manufacturer" treatment.
- **Host:** Windows polls the endpoint every 8 ms, and each report is delivered
  as a raw-input event with its exact counts. Pointer acceleration doesn't
  matter, because raw input sees the device's counts.
- **Generators:** `square` and `realistic` are the two wiggler modes. The
  `baseline:*` rows are simple jigglers that should be caught, as a check on
  the harness: `naive` is this project's first Arduino version (4 px right and
  back every 5 s), and `circle` moves in a continuous circle.
- **Scenarios:**
  - *not calibrated*: the user skipped the app's calibration step.
  - *calibrated, wiggler is 2nd mouse*: the user calibrated with their real
    mouse and the wiggler is plugged in alongside it. This is the usual case.
  - *wiggler is the calibrated mouse*: calibration was done by moving the
    wiggler. This only uses the stricter primary-mouse detectors.

Not simulated: a person using a real mouse at the same time, touchpads, and
the app's 5 s cooldown after a detection (each run stops at the first
detection).

## Building and running

Requires CMake and a C/C++17 compiler (GCC, Clang or MSVC).

```sh
cmake -S . -B build
cmake --build build
./build/jiggler_detection_test                 # 30 min x 5 seeds per row
./build/jiggler_detection_test --minutes 120 --seeds 20
./build/jiggler_detection_test --vid 2E8A      # as a stock Raspberry Pi Pico
```

The exit status is 1 if either wiggler mode was detected in any run, so
`ctest --test-dir build` passes only once both modes go undetected.

## Reading the output

Each row shows how many runs were flagged, the earliest detection time, the
highest behaviour/combined suspicion scores seen, and the reasons the
detector gave. To tune the wiggler, change the parameters in
`../rp2040-one-mouse-wiggler/lib/wiggle/wiggle.h` or the generator code in
`wiggle.c` and re-run.
