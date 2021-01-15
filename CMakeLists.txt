cmake_minimum_required(VERSION 3.10)
project(enc_lab)
set(CMAKE_CXX_STANDARD 17)

#Let the compiler know where to find the Intel Media SDK
set(MFX_HOME "/opt/intel/mediasdk")
set(MFX_INCLUDES "${MFX_HOME}/include/mfx")
set(MFX_LIBRARY  "${MFX_HOME}/lib/libmfx.so")

#Let the compiler know where to find the header files
include_directories(${MFX_INCLUDES})

#Build the encoder
add_executable(enc_lab encode.cpp)
target_link_libraries(enc_lab ${MFX_LIBRARY} va-drm va fdk-aac dl)