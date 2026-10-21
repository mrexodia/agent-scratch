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

enable_testing()
add_test(NAME wiggler_not_detected COMMAND jiggler_detection_test)
