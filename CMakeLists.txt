cmake_minimum_required(VERSION 3.10)
project(Fern)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(Fern WIN32 
    src/main.cpp 
    src/capture.cpp
    src/encoder.cpp
    src/buffer.cpp
    src/ipc_server.cpp
    src/hotkey.cpp
    src/dump.cpp
    src/isolatedAudioCapture.cpp
    include/fern/capture.h
    include/fern/encoder.h
    include/fern/buffer.h
    include/fern/ipc_server.h
    include/fern/hotkey.h
    include/fern/dump.h
    include/fern/isolatedAudioCapture.h)

target_link_libraries(Fern d3d11 dxgi mfplat mfuuid mfreadwrite mmdevapi RuntimeObject)
