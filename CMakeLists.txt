cmake_minimum_required(VERSION 3.10)
project(mod_grpc_api CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 1. Find Main Dependencies
find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)
find_package(Protobuf REQUIRED)

# Use pkg-config for the core gRPC libraries
pkg_check_modules(GRPC REQUIRED grpc++ grpc)

# MANUALLY find the reflection library because the .pc file is often missing
find_library(GRPC_REFLECTION_LIB NAMES grpc++_reflection REQUIRED)

# 2. FreeSWITCH & Plugin Headers
include_directories(
    /usr/local/freeswitch/include/freeswitch
    ${GRPC_INCLUDE_DIRS}
    ${Protobuf_INCLUDE_DIRS}
    ${CMAKE_CURRENT_BINARY_DIR}
)

# 3. Protobuf & gRPC Generation
set(proto_file "${CMAKE_CURRENT_SOURCE_DIR}/freeswitch_api.proto")

find_program(PROTOC_EXECUTABLE protoc)
find_program(GRPC_CPP_PLUGIN NAMES grpc_cpp_plugin)

add_custom_command(
    OUTPUT "freeswitch_api.pb.cc" "freeswitch_api.pb.h" "freeswitch_api.grpc.pb.cc" "freeswitch_api.grpc.pb.h"
    COMMAND ${PROTOC_EXECUTABLE}
            --cpp_out=.
            --grpc_out=.
            --plugin=protoc-gen-grpc=${GRPC_CPP_PLUGIN}
            -I ${CMAKE_CURRENT_SOURCE_DIR}
            ${proto_file}
    DEPENDS ${proto_file}
)

# 4. Build the Module
add_library(mod_grpc_api SHARED 
    mod_grpc_api.cpp 
    freeswitch_api.pb.cc 
    freeswitch_api.grpc.pb.cc
)

# 5. Link Libraries
target_link_libraries(mod_grpc_api
    ${GRPC_LIBRARIES}
    ${GRPC_REFLECTION_LIB}     # Linked manually here
    ${Protobuf_LIBRARIES}
    ${CMAKE_THREAD_LIBS_INIT}
    dl
)

# 6. Formatting for FreeSWITCH
set_target_properties(mod_grpc_api PROPERTIES PREFIX "")

# 7. Installation
install(TARGETS mod_grpc_api DESTINATION /usr/local/freeswitch/mod)
