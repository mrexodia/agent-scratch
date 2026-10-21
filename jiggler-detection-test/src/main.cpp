// Runs the wiggler's movement generators (rp2040-one-mouse-wiggler/lib/wiggle)
// through the Detect My Jiggler detection algorithms in simulated time and
// reports whether, when and why they get flagged.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "detector.h"
#include "wiggle.h"

// --- Movement generators -------------------------------------------------------

static uint64_t rng_state;

static uint32_t rand32(void) {
    // xorshift64*
    rng_state ^= rng_state >> 12;
    rng_state ^= rng_state << 25;
    rng_state ^= rng_state >> 27;
    return (uint32_t)((rng_state * 0x2545F4914F6CDD1DULL) >> 32);
}

struct Generator {
    virtual ~Generator() = default;
    // Called at every host poll; returns true to send a report.
    virtual bool Poll(uint32_t t_ms, int8_t& dx, int8_t& dy) = 0;
};

struct WiggleGenerator : Generator {
    wiggle_t w;
    explicit WiggleGenerator(wiggle_mode_t mode) {
        wiggle_init(&w, rand32);
        wiggle_start(&w, mode, 0);
    }
    bool Poll(uint32_t t_ms, int8_t& dx, int8_t& dy) override {
        return wiggle_poll(&w, t_ms, &dx, &dy);
    }
};

// The first Arduino version of the wiggler: 4 px right and back every 5 s.
struct NaiveGenerator : Generator {
    bool Poll(uint32_t t_ms, int8_t& dx, int8_t& dy) override {
        uint32_t phase = t_ms % 5000;
        if (phase < 8) { dx = 4; dy = 0; return true; }
        if (phase >= 48 && phase < 56) { dx = -4; dy = 0; return true; }
        return false;
    }
};

// A classic cheap jiggler: continuously moving in a circle.
struct CircleGenerator : Generator {
    double sent_x = 0, sent_y = 0;
    bool Poll(uint32_t t_ms, int8_t& dx, int8_t& dy) override {
        double a = t_ms / 1000.0 * 2.0 * M_PI / 4.0;  // one turn every 4 s
        double x = std::round(40 * std::cos(a)), y = std::round(40 * std::sin(a));
        dx = (int8_t)(x - sent_x);
        dy = (int8_t)(y - sent_y);
        sent_x = x;
        sent_y = y;
        return dx || dy;
    }
};

struct GeneratorInfo {
    const char* name;
    bool is_wiggler;  // false: a baseline that is expected to be detected
    std::function<std::unique_ptr<Generator>()> make;
};

static const std::vector<GeneratorInfo> kGenerators = {
    {"square", true, [] { return std::make_unique<WiggleGenerator>(WIGGLE_MODE_SQUARE); }},
    {"realistic", true, [] { return std::make_unique<WiggleGenerator>(WIGGLE_MODE_REALISTIC); }},
    {"baseline:naive", false, [] { return std::make_unique<NaiveGenerator>(); }},
    {"baseline:circle", false, [] { return std::make_unique<CircleGenerator>(); }},
};

// --- Scenarios -----------------------------------------------------------------

struct Scenario {
    const char* name;
    bool primary_identified;
    bool is_primary;
};

static const Scenario kScenarios[] = {
    {"not calibrated", false, false},
    {"calibrated, wiggler is 2nd mouse", true, false},
    {"wiggler is the calibrated mouse", true, true},
};

// --- Simulation ----------------------------------------------------------------

struct RunResult {
    bool detected = false;
    uint32_t detected_ms = 0;
    std::string reason;
    int max_behavior = 0;
    int max_combined = 0;
    uint32_t reports = 0;
};

static RunResult Simulate(const GeneratorInfo& gen, const Scenario& sc, const std::string& vid,
                          uint64_t seed, uint32_t duration_ms, uint32_t poll_ms) {
    rng_state = seed * 0x9E3779B97F4A7C15ULL + 1;
    auto g = gen.make();

    DetectorConfig cfg;
    cfg.vid = vid;
    cfg.primaryIdentified = sc.primary_identified;
    cfg.isPrimary = sc.is_primary;
    Detector det(cfg);

    RunResult r;
    uint32_t next_check = 200;
    for (uint32_t t = 0; t <= duration_ms; t += poll_ms) {
        int8_t dx = 0, dy = 0;
        if (g->Poll(t, dx, dy)) {
            det.OnMove(t, dx, dy);
            r.reports++;
        }
        while (next_check <= t) {
            DetectionResult res;
            bool hit = det.Check(next_check, res);
            r.max_behavior = std::max(r.max_behavior, det.device().behaviorSuspicionScore);
            r.max_combined = std::max(r.max_combined, det.device().combinedSuspicionScore);
            if (hit) {
                r.detected = true;
                r.detected_ms = next_check;
                r.reason = res.reason;
                return r;
            }
            next_check += 200;
        }
    }
    return r;
}

static void Usage(const char* argv0) {
    std::printf(
        "usage: %s [--minutes N] [--seeds N] [--vid XXXX] [--poll-ms N]\n"
        "  --minutes N   simulated time per run (default 30)\n"
        "  --seeds N     runs per generator/scenario (default 5)\n"
        "  --vid XXXX    USB vendor ID the detector sees (default 046D, Logitech)\n"
        "  --poll-ms N   host polling interval (default 8: Windows polls a\n"
        "                full-speed bInterval 10 endpoint every 8 ms)\n"
        "Exit status is 1 if a wiggler mode was detected in any run.\n",
        argv0);
}

int main(int argc, char** argv) {
    uint32_t minutes = 30, seeds = 5, poll_ms = 8;
    std::string vid = "046D";
    for (int i = 1; i < argc; i++) {
        auto arg = [&](const char* name) { return !std::strcmp(argv[i], name) && i + 1 < argc; };
        if (arg("--minutes")) minutes = (uint32_t)std::atoi(argv[++i]);
        else if (arg("--seeds")) seeds = (uint32_t)std::atoi(argv[++i]);
        else if (arg("--vid")) vid = argv[++i];
        else if (arg("--poll-ms")) poll_ms = (uint32_t)std::atoi(argv[++i]);
        else { Usage(argv[0]); return 2; }
    }
    if (!minutes || !seeds || !poll_ms) { Usage(argv[0]); return 2; }
    for (auto& c : vid) c = (char)std::toupper((unsigned char)c);

    std::printf("Detect My Jiggler simulation: VID %s (%s), %u min x %u seeds per row, %u ms polling\n\n",
                vid.c_str(), GetVIDTrustLevelString(GetVIDTrustLevel(vid)), minutes, seeds, poll_ms);
    std::printf("%-16s %-34s %-9s %-10s %-9s %s\n", "generator", "scenario", "detected", "first at",
                "max score", "reason");
    std::printf("%s\n", std::string(120, '-').c_str());

    bool wiggler_detected = false;
    bool baseline_missed = false;
    for (const auto& gen : kGenerators) {
        for (const auto& sc : kScenarios) {
            uint32_t hits = 0, first_ms = UINT32_MAX;
            int max_behavior = 0, max_combined = 0;
            std::set<std::string> reasons;
            for (uint32_t s = 0; s < seeds; s++) {
                RunResult r = Simulate(gen, sc, vid, s, minutes * 60000u, poll_ms);
                max_behavior = std::max(max_behavior, r.max_behavior);
                max_combined = std::max(max_combined, r.max_combined);
                if (r.detected) {
                    hits++;
                    first_ms = std::min(first_ms, r.detected_ms);
                    reasons.insert(r.reason);
                }
            }

            char detected[16], first[16], score[16];
            std::snprintf(detected, sizeof detected, "%u/%u", hits, seeds);
            if (hits) std::snprintf(first, sizeof first, "%.1f s", first_ms / 1000.0);
            else std::snprintf(first, sizeof first, "-");
            std::snprintf(score, sizeof score, "%d/%d", max_behavior, max_combined);
            std::string reason;
            for (const auto& rs : reasons) reason += (reason.empty() ? "" : " | ") + rs;
            std::printf("%-16s %-34s %-9s %-10s %-9s %s\n", gen.name, sc.name, detected, first, score,
                        reason.c_str());

            if (gen.is_wiggler && hits) wiggler_detected = true;
            if (!gen.is_wiggler && !std::strcmp(gen.name, "baseline:circle") && hits < seeds) baseline_missed = true;
        }
    }

    std::printf("\nmax score = highest behaviour / combined (VID + behaviour) suspicion score seen.\n");
    if (baseline_missed) {
        std::printf("WARNING: the circle baseline was not always detected; check the harness.\n");
    }
    std::printf("%s\n", wiggler_detected ? "RESULT: wiggler DETECTED" : "RESULT: wiggler not detected");
    return wiggler_detected ? 1 : 0;
}
