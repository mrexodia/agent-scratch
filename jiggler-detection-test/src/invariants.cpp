// Generator contract tests, independent of the detector's alert thresholds.
#include "wiggle.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <vector>

#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #c); std::exit(1); } } while (0)

static uint64_t state;
static int rng_kind;
static uint32_t random32() {
    if (rng_kind == 1) return 0;
    if (rng_kind == 2) return UINT32_MAX;
    state ^= state >> 12;
    state ^= state << 25;
    state ^= state >> 27;
    return uint32_t((state * 0x2545F4914F6CDD1DULL) >> 32);
}

struct Report { uint32_t time; int x, y; };
static std::vector<Report> exercise(wiggle_mode_t mode, unsigned seed, uint32_t poll,
                                     uint32_t origin, bool stalls, int kind = 0) {
    state = uint64_t(seed) * 0x9E3779B97F4A7C15ULL + 1;
    rng_kind = kind;
    wiggle_t w;
    wiggle_init(&w, random32);
    wiggle_start(&w, mode, origin);
    std::vector<Report> reports;
    std::deque<int> directions, magnitudes;
    int x = 0, y = 0, direction = 0, direction_run = 0;
    uint32_t last = 0, continuous = 0;
    const int bound = mode == WIGGLE_MODE_QUIET ? WIGGLE_QUIET_BOUND : WIGGLE_SWEEP_BOUND;
    for (uint32_t t = 0; t < 600000; t += poll) {
        // Simulate the host not accepting polls. No elapsed-time catch-up.
        if (stalls && t % 19000 >= 4000 && t % 19000 < 9000) continue;
        int8_t dx = 99, dy = 99;
        if (!wiggle_poll(&w, origin + t, &dx, &dy)) {
            CHECK(dx == 0 && dy == 0);
            continue;
        }
        CHECK(dx != -128 && dy != -128 && (dx || dy));
        CHECK(dx * w.axis_y == dy * w.axis_x);
        int magnitude = std::max(std::abs(int(dx)), std::abs(int(dy)));
        CHECK(magnitude >= 4 && magnitude <= (mode == WIGGLE_MODE_QUIET ? 28 : 72));
        int d = ((w.axis_x ? dx * w.axis_x : dy * w.axis_y) > 0) ? 1 : -1;
        if (direction && direction != d) {
            CHECK(direction_run >= 6);
            direction_run = 0;
        }
        direction = d;
        ++direction_run;
        x += dx; y += dy;
        CHECK(std::abs(x) <= bound && std::abs(y) <= bound);
        CHECK(x == w.position * w.axis_x && y == w.position * w.axis_y);
        if (!reports.empty()) {
            CHECK(t - last >= (mode == WIGGLE_MODE_QUIET ? 3200u : 10u));
            continuous = t - last < 2000 ? continuous + t - last : 0;
            CHECK(continuous < 30000);
        }
        last = t;
        reports.push_back({t, dx, dy});
        directions.push_back(d);
        magnitudes.push_back(magnitude);
        if (directions.size() > 50) directions.pop_front();
        if (magnitudes.size() > 100) magnitudes.pop_front();
        unsigned reversals = 0;
        for (size_t i = 1; i < directions.size(); ++i)
            reversals += directions[i] != directions[i-1];
        CHECK(reversals < 10);
        if (magnitudes.size() >= 12) {
            double mean = 0, variance = 0;
            for (int m : magnitudes) mean += m;
            mean /= magnitudes.size();
            for (int m : magnitudes) variance += (m - mean) * (m - mean);
            CHECK(std::sqrt(variance / magnitudes.size()) / mean > 0.30);
        }
        int8_t again_x = 99, again_y = 99;
        CHECK(!wiggle_poll(&w, origin + t, &again_x, &again_y));
        CHECK(again_x == 0 && again_y == 0);
    }
    CHECK(reports.size() >= 20); // Cannot pass by emitting no useful movement.
    CHECK(reports.front().time >= 500);

    // Restart discards a partial run and establishes a new origin, but must
    // preserve the line even when switching modes or resuming the USB device.
    int axis_x = w.axis_x, axis_y = w.axis_y;
    wiggle_start(&w, mode == WIGGLE_MODE_QUIET ? WIGGLE_MODE_SWEEP : WIGGLE_MODE_QUIET,
                 origin + 600000);
    CHECK(w.axis_x == axis_x && w.axis_y == axis_y);
    CHECK(w.position == 0 && w.count == 0 && w.index == 0);
    int8_t dx, dy;
    CHECK(!wiggle_poll(&w, origin + 600000, &dx, &dy));
    wiggle_start(&w, WIGGLE_MODE_COUNT, origin + 600000);
    CHECK(!wiggle_poll(&w, origin + 620000, &dx, &dy));
    CHECK(dx == 0 && dy == 0);
    return reports;
}

int main() {
    unsigned cases = 0;
    for (auto mode : {WIGGLE_MODE_QUIET, WIGGLE_MODE_SWEEP}) {
        for (uint32_t poll : {1u, 8u, 10u, 16u, 1000u}) {
            for (unsigned seed = 0; seed < 16; ++seed) {
                for (bool stalls : {false, true}) {
                    auto normal = exercise(mode, seed, poll, 0, stalls);
                    auto wrapped = exercise(mode, seed, poll, UINT32_MAX - 5000, stalls);
                    CHECK(normal.size() == wrapped.size());
                    for (size_t i = 0; i < normal.size(); ++i) {
                        CHECK(normal[i].time == wrapped[i].time);
                        CHECK(normal[i].x == wrapped[i].x && normal[i].y == wrapped[i].y);
                    }
                    cases += 2;
                }
            }
        }
        // Even pathological random sources must not hang, escape the bounds,
        // produce tiny/constant steps or defeat rollover-safe scheduling.
        for (int kind : {1, 2}) {
            exercise(mode, 0, 8, UINT32_MAX - 5000, true, kind);
            ++cases;
        }
    }
    std::printf("PASS: %u generator cases (bounds, cadence, variance, reversals, stalls, rollover, restart)\n", cases);
}
