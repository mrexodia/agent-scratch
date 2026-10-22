cmake_minimum_required(VERSION 3.13)
project(jiggler_detection_test C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)

# The wiggler's movement generators, built for the host.
add_subdirectory(../rp2040-one-mouse-wiggler/lib/wiggle wiggle)

add_executable(jiggler_detection_test
    src/main.cpp
    src/detector.cpp
    src/extract_vid_pid.cpp
    third_party/detect_my_jiggler/detection.cpp
)
# compat/ first so the vendored code's <windows.h> resolves to the stand-in.
target_include_directories(jiggler_detection_test PRIVATE
    compat
    src
    third_party/detect_my_jiggler
)
if(MSVC)
    target_compile_definitions(jiggler_detection_test PRIVATE _USE_MATH_DEFINES)
endif()
target_link_libraries(jiggler_detection_test PRIVATE wiggle)

add_executable(wiggle_invariants src/invariants.cpp)
target_link_libraries(wiggle_invariants PRIVATE wiggle)

enable_testing()
add_test(NAME wiggler_not_detected COMMAND jiggler_detection_test)
add_test(NAME wiggle_invariants COMMAND wiggle_invariants)
# A different VID has different thresholds and must not silently inherit the
# success claim for the firmware's existing 046D identity.
add_test(NAME stock_vid_detected COMMAND jiggler_detection_test --vid 2E8A --seeds 1)
set_tests_properties(stock_vid_detected PROPERTIES WILL_FAIL TRUE)

option(WIGGLE_EXTENDED_TESTS "Run two-hour holdouts at five polling intervals" OFF)
if(WIGGLE_EXTENDED_TESTS)
    foreach(poll 1 8 10 16 32)
        add_test(NAME holdout_${poll}ms COMMAND jiggler_detection_test
                 --minutes 120 --seeds 20 --seed-start 1000 --poll-ms ${poll})
    endforeach()
endif()
