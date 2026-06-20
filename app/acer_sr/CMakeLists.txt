cmake_minimum_required(VERSION 3.21)
project(moonlight_acer_sr CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

set(SR_SDK_DIR "C:/Apps/SR-Loom/lib/Simulated Reality/LeiaSR-SDK-1.36.2-win64"
    CACHE PATH "LeiaSR SDK root")
set(ORT_OPENVINO_DIR "C:/Apps/build-tools/nuget-packages/Intel.ML.OnnxRuntime.OpenVino.1.24.1"
    CACHE PATH "Intel.ML.OnnxRuntime.OpenVino NuGet package root")

file(GLOB SR_IMPORT_LIBS "${SR_SDK_DIR}/lib/*.lib")
if(NOT SR_IMPORT_LIBS)
    message(FATAL_ERROR "No SR import libraries found in ${SR_SDK_DIR}/lib")
endif()

set(ORT_OPENVINO_NATIVE_DIR "${ORT_OPENVINO_DIR}/runtimes/win-x64/native")
if(NOT EXISTS "${ORT_OPENVINO_NATIVE_DIR}/onnxruntime.lib")
    message(FATAL_ERROR "onnxruntime.lib not found in ${ORT_OPENVINO_NATIVE_DIR}")
endif()

set(SR_THIRD_PARTY_LIBS
    "${SR_SDK_DIR}/third_party/GLog/lib/x64/glog.lib"
    "${SR_SDK_DIR}/third_party/OpenCV/lib/x64/opencv_world343.lib"
    "${SR_SDK_DIR}/third_party/Leap/lib/LeapC.lib")

add_library(moonlight_acer_sr SHARED
    moonlight_acer_sr.cpp
)

add_executable(moonlight_acer_sr_smoke
    smoke_test.cpp
)

set_target_properties(moonlight_acer_sr PROPERTIES
    OUTPUT_NAME "moonlight-acer-sr"
)

set_target_properties(moonlight_acer_sr_smoke PROPERTIES
    OUTPUT_NAME "moonlight-acer-sr-smoke"
)

target_compile_definitions(moonlight_acer_sr PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _CRT_SECURE_NO_WARNINGS
)

target_compile_definitions(moonlight_acer_sr_smoke PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

if(MSVC)
    target_compile_options(moonlight_acer_sr PRIVATE /wd4005)
endif()

target_include_directories(moonlight_acer_sr PRIVATE
    "${ORT_OPENVINO_DIR}/build/native/include"
    "${SR_SDK_DIR}/include"
    "${SR_SDK_DIR}/third_party/asio/include"
    "${SR_SDK_DIR}/third_party/rapidxml"
    "${SR_SDK_DIR}/third_party/GLog/include"
    "${SR_SDK_DIR}/third_party/Leap/include"
    "${SR_SDK_DIR}/third_party/OpenCV/include"
)

target_link_libraries(moonlight_acer_sr PRIVATE
    ${SR_IMPORT_LIBS}
    ${SR_THIRD_PARTY_LIBS}
    "${ORT_OPENVINO_NATIVE_DIR}/onnxruntime.lib"
    d3d11
    d3dcompiler
    dxgi
    dxguid
    user32
    gdi32
)

target_link_libraries(moonlight_acer_sr_smoke PRIVATE
    d3d11
)
