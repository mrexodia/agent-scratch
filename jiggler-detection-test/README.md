# Jiggler detection test

Feeds the firmware's actual shared C movement engine
(`../rp2040-one-mouse-wiggler/lib/wiggle`) through the algorithms from
[Detect My Jiggler](https://github.com/detectmyjiggler/detect_my_jiggler).
Everything runs in simulated time on the host; no hardware or Windows is needed.
This tests a particular detector, **not indistinguishability from a real mouse**.

## What is simulated

- **Detector:** unmodified files in `third_party/detect_my_jiggler` (MIT; see
  `UPSTREAM.md`). The original per-device bookkeeping, thresholds and alert
  decisions from the app's `main.cpp` are ported in `src/detector.cpp`.
- **Identity:** the firmware's existing VID/PID `046D:C077`, treated as a verified
  manufacturer. No VID/descriptor changes are part of these movement changes.
- **Host:** by default, polls every 8 ms and delivers each report as a distinct
  Raw Input event with exact counts. Pointer acceleration is irrelevant to raw
  counts. Other poll intervals are configurable. Polls and the 200 ms detector
  checks execute in chronological order, including when their periods differ.
- **Modes:** `quiet` replaces saved mode 0 (`square`); `sweep` replaces saved
  mode 1 (`realistic`). Both share a fixed line selected at boot. Quiet moves
  one step per rest; sweep emits short variable-speed runs followed by rests.
  The names describe the new behavior, not a claim of human-like motion.
- **Controls:** `baseline:naive` is the original 4-count out/back jiggler;
  `baseline:circle` moves continuously in a circle. A missed circle control is
  a **test failure**, not merely a warning.
- **Scenarios:** no calibration; a separately calibrated primary mouse with
  the wiggler as secondary; and the wiggler itself designated primary.

Not modeled: physical USB enumeration/submission errors, report merging/loss,
touchpad activity, pre-existing history, repeated manual mode cycling, or the
app's post-detection cooldown. Each detector run stops at its first detection.
The generator contract tests separately exercise late polls and state resets.
Pauses **do not** clear the detector histories; secondary-device scoring still
runs on stale histories, just as in the original app.

## Building and running

Requires CMake and a C/C++17 compiler (GCC, Clang or MSVC):

```sh
cmake -S . -B build
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

For single-configuration generators, run `build/jiggler_detection_test` directly.
For Visual Studio, the path is `build/Release/jiggler_detection_test.exe`:

```sh
./build/Release/jiggler_detection_test.exe
./build/Release/jiggler_detection_test.exe --minutes 120 --seeds 20 --seed-start 1000 --poll-ms 8
./build/Release/jiggler_detection_test.exe --vid 2E8A --seeds 1  # expected detection / exit 1
```

`--minutes`, `--seeds` and `--poll-ms` must be positive integers. `--seed-start`
allows zero. Invalid arguments, malformed VIDs and time overflow are rejected
with exit status 2. Exit status 1 means **any mode was detected in any run**, the
circle control was missed, or a non-detected wiggler produced fewer than two
reports per simulated minute. Neither mode has an advisory/exempted alert path.

Enable the complete holdout matrix as CTest tests:

```sh
cmake -S . -B build -DWIGGLE_EXTENDED_TESTS=ON
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure -j 2
```

Default CTest includes the strict detection test, generator invariants, and a
stock-Pico-VID control whose expected result is detection. The extended option
adds two-hour runs over 20 holdout seeds at each of 1, 8, 10, 16 and 32 ms polling.

## Results and rationale

Reviewed detector revision: `b638ce84c14ef524816c8bb009de7723b7afcd63`.

Before changes, the default test caught the old square in 5/5 uncalibrated and
secondary runs, and old realistic movement in 3/5 uncalibrated and 5/5 secondary
runs. After changes:

| Validation | Result |
| --- | --- |
| Default: 30 minutes × 5 seeds × 2 modes × 3 scenarios | No detections |
| Holdout: 120 minutes × 20 seeds × 2 modes × 3 scenarios × 5 poll intervals | No detections |
| Highest non-primary behavior / combined score | 60 / 36 |
| Highest primary score | 0 |
| Circle control, all tested scenarios/poll intervals | Detected |
| Stock Pico VID `2E8A`, one seed, 30 minutes | Both modes detected in non-primary scenarios |
| Generator invariants | 644 cases passed |

The holdout seeds are 1000–1019, separate from the default 0–4. That is 600
mode/scenario/poll cases, totaling 1,200 simulated wiggler-hours (not 600
independent RNG seeds). Results are not a hardware certification.

The movement policy uses properties of **emitted reports**, rather than hoping
that random pauses or trajectories happen to pass:

- All integer positions lie on one fixed horizontal, vertical or diagonal line.
  Circle fitting is degenerate on such points, so it cannot accumulate arc
  evidence. The axis persists through restarts to avoid introducing corners.
- Each active-axis magnitude is at least four counts. There are no unit-step
  micro-movements or small <=3-count oscillations.
- Low/high magnitude pairs are separated: quiet uses 4–7 vs 16–28; sweep uses
  4–12 vs 24–72. Rolling magnitude variation remains above the detector's
  constant-speed/delta thresholds. Its “speed” is report magnitude, **not**
  distance divided by elapsed time; timestamp jitter alone is insufficient.
- A direction run contains at least six reports, so a 50-report direction
  history contains at most nine reversals, below the oscillation threshold.
- Quiet rests between every report; sweep rests between every 6–8 reports.
  Rests are at least 3.2 seconds, preventing continuous 30-second movement.
- The geometric and alternating-direction checks can still fire, contributing
  30 points each. For the existing allowlisted identity their total (60) stays
  below the secondary behavior threshold (75) and uncalibrated threshold (90).
  Weighted combined score is 36, also below the relevant thresholds.

This argument assumes uninterrupted runs with exact per-report delivery, as in
the simulation. Dropped/coalesced reports or repeatedly interrupted runs can
break the sample-count/variance properties. Other identities have lower alert
thresholds, and other detectors could explicitly recognize the fixed-line and
low/high-report pattern. Do not describe this as universal undetectability.

`src/invariants.cpp` checks HID ranges, nonzero/lively output, bounded raw travel,
collinearity, minimum cadence, magnitude CV, reversal limits, no catch-up on late
polls, deterministic wraparound behavior, reset/axis retention, invalid modes,
and constant-zero/all-ones random sources. It does not change detector scoring.

Before deploying, verify real Raw Input traces, no-click/no-wheel reports,
BOOT controls and persistence, failed USB submission retries, and suspend/
resume behavior on the target board. The current UF2 build has not been flashed.
