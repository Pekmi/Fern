cmake_minimum_required(VERSION 3.10)
if(POLICY CMP0091)
    cmake_policy(SET CMP0091 NEW)
endif()
project(Fern)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(Fern WIN32 
    src/main.cpp 
    src/clock.cpp
    src/capture_feedback.cpp
    src/capture_session.cpp
    src/capture.cpp
    src/encoder.cpp
    src/buffer.cpp
    src/settings.cpp
    src/logger.cpp
    src/ipc_server.cpp
    src/hotkey.cpp
    src/dump.cpp
    src/replay_export.cpp
    src/multi_app_audio_capture.cpp
    src/isolatedAudioCapture.cpp
    include/fern/audio_activity.h
    include/fern/clock.h
    include/fern/capture_feedback.h
    include/fern/capture_session.h
    include/fern/capture.h
    include/fern/encoder.h
    include/fern/buffer.h
    include/fern/settings.h
    include/fern/logger.h
    include/fern/ipc_server.h
    include/fern/hotkey.h
    include/fern/dump.h
    include/fern/replay_export.h
    include/fern/multi_app_audio_capture.h
    include/fern/isolatedAudioCapture.h)

target_link_libraries(Fern d3d11 dxgi mfplat mfuuid mfreadwrite mmdevapi RuntimeObject shell32 ole32 winmm user32 gdi32 psapi)

if(MSVC)
    set_property(TARGET Fern PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()
