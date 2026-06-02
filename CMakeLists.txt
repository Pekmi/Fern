cmake_minimum_required(VERSION 3.10)
project(Fern)

add_executable(Fern 
    src/main.cpp 
    src/capture.cpp
    src/encoder.cpp
    src/buffer.cpp
    include/fern/capture.h
    include/fern/encoder.h
    include/fern/buffer.h)

target_link_libraries(Fern d3d11 dxgi mfplat mfuuid mfreadwrite)