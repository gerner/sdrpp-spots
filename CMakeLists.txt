cmake_minimum_required(VERSION 3.13)
project(sdrpp-spots)

file(GLOB SRC "src/*.cpp")

#set(SDRPP_MODULE_COMPILER_FLAGS -O3 -std=c++17 -Wno-unused-command-line-argument -undefined dynamic_lookup)
include(${SDRPP_MODULE_CMAKE})
