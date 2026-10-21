// A single mouse as seen by Detect My Jiggler, driven by simulated time.
#pragma once

#include <cstdint>
#include <string>

#include "mouse_device.h"

struct DetectorConfig {
    std::string vid = "046D";
    std::string pid = "C077";
    // Whether the user ran the app's calibration (clicked OK with their own
    // mouse), and whether this device is that calibrated "primary" mouse.
    bool primaryIdentified = false;
    bool isPrimary = false;
};

class Detector {
public:
    explicit Detector(const DetectorConfig& config);

    // One raw input report (WM_INPUT) from this device at time t_ms.
    void OnMove(uint32_t t_ms, int32_t dx, int32_t dy);

    // One pass of the app's detection loop, which runs every 200 ms.
    // Returns true when the device is flagged as a jiggler.
    bool Check(uint32_t t_ms, DetectionResult& result);

    const MouseDevice& device() const { return device_; }

private:
    DetectorConfig config_;
    MouseDevice device_;
};
