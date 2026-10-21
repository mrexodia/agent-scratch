// Port of the per-device parts of detect_my_jiggler/main.cpp: the WM_INPUT
// bookkeeping and the body of the DetectMouseJiggler() loop for one device.
// The Windows plumbing (Raw Input, touchpad filtering, popups and the 5 s
// cooldown after a detection) is left out; the logic and thresholds are kept
// as-is. Keep this in sync with third_party/detect_my_jiggler/UPSTREAM.md.

#include "detector.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <sstream>

#include "detection.h"

using Clock = std::chrono::steady_clock;

// Simulated time. Offset from the epoch because the app treats a
// default-constructed time_point as "unset".
static Clock::time_point SimTime(uint32_t t_ms) {
    return Clock::time_point(std::chrono::hours(1) + std::chrono::milliseconds(t_ms));
}

Detector::Detector(const DetectorConfig& config)
    : config_(config),
      device_("\\\\?\\HID#VID_" + config.vid + "&PID_" + config.pid + "#7&1a2b3c4d&0&0000#{378de44c-56ef-11d1-bc8c-00a0c91405dd}") {
    device_.lastMovementTime = SimTime(0);
}

void Detector::OnMove(uint32_t t_ms, int32_t dx, int32_t dy) {
    if (dx == 0 && dy == 0) return;
    auto now = SimTime(t_ms);
    auto& device = device_;

    // Record when this device first started moving
    if (device.firstMovementTime == Clock::time_point()) {
        device.firstMovementTime = now;
    }

    // Update continuous movement tracking
    double timeSinceLastMove = std::chrono::duration<double>(now - device.lastMovementTime).count();
    if (timeSinceLastMove < PAUSE_THRESHOLD_SECONDS) {
        device.continuousDuration += timeSinceLastMove;
    } else {
        device.continuousDuration = 0.0;
    }
    device.lastMovementTime = now;

    // Calculate and store speed for constant speed detection
    double speed = sqrt((double)(dx * dx + dy * dy));
    device.speedHistory.push_back(speed);
    if (device.speedHistory.size() > MAX_SPEED_HISTORY_SIZE) {
        device.speedHistory.pop_front();
    }

    // Update position history
    POINT last = {0, 0};
    if (!device.movements.empty()) {
        last = device.movements.back();
    }
    POINT next = { last.x + (LONG)dx, last.y + (LONG)dy };
    device.movements.push_back(next);

    // Store for pattern detection
    device.patternHistory.push_back(next);
    if (device.patternHistory.size() > MAX_PATTERN_HISTORY_SIZE) {
        device.patternHistory.pop_front();
    }

    // Track direction for oscillation detection
    int dirX = (dx > 0) ? 1 : ((dx < 0) ? -1 : 0);
    int dirY = (dy > 0) ? 1 : ((dy < 0) ? -1 : 0);
    device.directionXHistory.push_back(dirX);
    device.directionYHistory.push_back(dirY);
    if (device.directionXHistory.size() > MAX_DIRECTION_HISTORY_SIZE) {
        device.directionXHistory.pop_front();
        device.directionYHistory.pop_front();
    }

    // Bound movement history to keep UI responsive
    auto &mv = device.movements;
    const size_t maxHistory = 200;
    while (mv.size() > maxHistory) {
        mv.pop_front();
    }
}

bool Detector::Check(uint32_t t_ms, DetectionResult& result) {
    auto now = SimTime(t_ms);
    auto& device = device_;
    const bool primaryMouseIdentified = config_.primaryIdentified;
    const bool isPrimaryMouse = config_.primaryIdentified && config_.isPrimary;

    // Skip devices with insufficient movement data
    size_t minMovements = 30;
    if (device.vidTrustLevel == VIDTrustLevel::BLOCKLIST_CRITICAL ||
        device.vidTrustLevel == VIDTrustLevel::BLOCKLIST_HIGH) {
        minMovements = 3;
    } else if (device.vidTrustLevel == VIDTrustLevel::BLOCKLIST_MEDIUM ||
               device.vidTrustLevel == VIDTrustLevel::UNKNOWN) {
        minMovements = 5;
    }
    if (primaryMouseIdentified && !isPrimaryMouse && minMovements > 5) {
        minMovements = 5;
    }
    if (isPrimaryMouse) {
        minMovements = PRIMARY_MOUSE_MIN_MOVEMENTS;
    }
    if (device.movements.size() < minMovements) return false;

    // Wait minimum movement observation time before analysis.
    if (device.firstMovementTime == Clock::time_point()) return false;
    double elapsed = std::chrono::duration<double>(now - device.firstMovementTime).count();
    double minElapsed = isPrimaryMouse ? PRIMARY_MOUSE_MIN_DETECTION_SECONDS : MIN_DETECTION_SECONDS;
    if (elapsed < minElapsed) return false;

    if (isPrimaryMouse) {
        const bool hasNewData =
            (device.lastMovementTime != device.lastSeenMovementTime);
        device.lastSeenMovementTime = device.lastMovementTime;

        if (!hasNewData) {
            device.arcHistory.clear();
            device.primaryConsecutiveHighScores =
                std::max(0, device.primaryConsecutiveHighScores - 1);
            device.primaryZigzagConsecutiveHighScores =
                std::max(0, device.primaryZigzagConsecutiveHighScores - 1);
            device.primaryRepetitiveConsecutiveScores =
                std::max(0, device.primaryRepetitiveConsecutiveScores - 1);
            return false;
        }

        DetectionResult primaryResult;
        bool primaryWheelDetected = DetectPrimaryMouseWheelPattern(device, primaryResult);

        DetectionResult zigzagResult;
        bool primaryZigzagDetected = DetectPrimaryMouseZigzagPattern(device, zigzagResult);

        DetectionResult repetitiveResult;
        bool primaryRepetitiveDetected = DetectPrimaryMouseRepetitiveDelta(device, repetitiveResult);

        int maxProgress = std::max({device.primaryConsecutiveHighScores,
                                    device.primaryZigzagConsecutiveHighScores,
                                    device.primaryRepetitiveConsecutiveScores});
        device.behaviorSuspicionScore = std::min(maxProgress * 4, 100);
        device.UpdateCombinedScore();

        if (!primaryWheelDetected && !primaryZigzagDetected && !primaryRepetitiveDetected) {
            return false;
        }

        device.isJiggler = true;
        DetectionResult& winningResult = primaryWheelDetected ? primaryResult :
            (primaryZigzagDetected ? zigzagResult : repetitiveResult);
        device.detectionReason = "Jiggler Detected: " + winningResult.reason;
        device.detectionType = winningResult.type;
        result = winningResult;
        result.reason = device.detectionReason;
        return true;
    }

    // Calculate behavioral suspicion score
    DetectionResult deviceResult;
    device.behaviorSuspicionScore = CalculateBehaviorSuspicionScore(device, deviceResult);
    device.UpdateCombinedScore();

    int threshold = 85;
    if (device.vidTrustLevel == VIDTrustLevel::BLOCKLIST_CRITICAL ||
        device.vidTrustLevel == VIDTrustLevel::BLOCKLIST_HIGH) {
        threshold = primaryMouseIdentified ? 40 : 50;
    } else if (device.vidTrustLevel == VIDTrustLevel::BLOCKLIST_MEDIUM) {
        threshold = primaryMouseIdentified ? 45 : 55;
    } else if (device.vidTrustLevel == VIDTrustLevel::UNKNOWN) {
        threshold = primaryMouseIdentified ? 50 : 60;
    } else if (primaryMouseIdentified && !isPrimaryMouse) {
        threshold = 75;
    }

    auto flag = [&](const std::string& reason, DetectionType type) {
        device.isJiggler = true;
        device.detectionReason = reason;
        device.detectionType = type;
        result.type = type;
        result.value = deviceResult.value;
        result.reason = reason;
        return true;
    };

    if (device.combinedSuspicionScore >= threshold) {
        std::ostringstream reason;
        reason << "Jiggler Detected: ";
        if (device.vidTrustLevel == VIDTrustLevel::BLOCKLIST_CRITICAL) {
            reason << "Known jiggler hardware (VID " << device.vid << ")";
        } else if (device.vidTrustLevel == VIDTrustLevel::BLOCKLIST_HIGH) {
            reason << "DIY/microcontroller hardware (VID " << device.vid << ")";
        } else if (!deviceResult.reason.empty()) {
            reason << deviceResult.reason;
        } else {
            reason << "Suspicious movement patterns";
        }
        return flag(reason.str(), deviceResult.type);
    }

    int highThresh = primaryMouseIdentified ? 15 : 25;
    if (device.vidTrustLevel == VIDTrustLevel::BLOCKLIST_HIGH && device.behaviorSuspicionScore >= highThresh) {
        return flag("Jiggler Detected: Suspicious hardware with abnormal patterns (VID " + device.vid + ")",
                    DetectionType::SMALL_MOVEMENT);
    }

    int medThresh = primaryMouseIdentified ? 25 : 35;
    if (device.vidTrustLevel == VIDTrustLevel::BLOCKLIST_MEDIUM && device.behaviorSuspicionScore >= medThresh) {
        return flag("Jiggler Detected: Microcontroller device with suspicious patterns (VID " + device.vid + ")",
                    DetectionType::SMALL_MOVEMENT);
    }

    int unknownThresh = primaryMouseIdentified ? 40 : 50;
    if (device.vidTrustLevel == VIDTrustLevel::UNKNOWN && device.behaviorSuspicionScore >= unknownThresh) {
        std::string reason = "Jiggler Detected: Unknown device with suspicious patterns";
        if (!device.vid.empty() && device.vid != "0000") {
            reason += " (VID " + device.vid + ")";
        }
        return flag(reason, deviceResult.type);
    }

    int allowThresh = primaryMouseIdentified ? 75 : 90;
    if (device.behaviorSuspicionScore >= allowThresh) {
        return flag("Jiggler Detected: " +
                    (deviceResult.reason.empty() ? std::string("Highly suspicious movement patterns") : deviceResult.reason),
                    deviceResult.type);
    }

    int critThresh = primaryMouseIdentified ? 10 : 20;
    if (device.vidTrustLevel == VIDTrustLevel::BLOCKLIST_CRITICAL && device.behaviorSuspicionScore >= critThresh) {
        return flag("Jiggler Detected: Known jiggler manufacturer (VID " + device.vid + ")",
                    DetectionType::SMALL_MOVEMENT);
    }

    return false;
}
