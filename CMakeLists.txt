cmake_minimum_required(VERSION 3.10)
project(Fern)

add_executable(Fern WIN32 
    src/main.cpp 
    src/capture.cpp
    src/encoder.cpp
    src/buffer.cpp
    src/ipc_server.cpp
    include/fern/capture.h
    include/fern/encoder.h
    include/fern/buffer.h
    include/fern/ipc_server.h)

target_link_libraries(Fern d3d11 dxgi mfplat mfuuid mfreadwrite)