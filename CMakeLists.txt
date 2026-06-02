cmake_minimum_required(VERSION 3.10)
project(Fern)

add_executable(Fern 
    src/main.cpp 
    src/capture.cpp
    src/encoder.cpp
    include/fern/capture.h
    include/fern/encoder.h)

target_link_libraries(Fern d3d11 dxgi mfplat mfuuid mfreadwrite)