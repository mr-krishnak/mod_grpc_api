cmake_minimum_required(VERSION 3.10)
project(mod_grpc_api CXX)

set(CMAKE_CXX_STANDARD 17)

# 1. Find Dependencies
find_package(Threads REQUIRED)
find_package(Protobuf REQUIRED)
find_package(gRPC CONFIG REQUIRED)

# 2. FreeSWITCH Headers (Adjust if your path is different)
# Often found in /usr/local/freeswitch/include/freeswitch or /usr/include/freeswitch
include_directories(/usr/local/freeswitch/include/freeswitch)
include_directories(${CMAKE_CURRENT_BINARY_DIR})

# 3. Protobuf & gRPC Generation
set(proto_file "${CMAKE_CURRENT_SOURCE_DIR}/freeswitch_api.proto")
find_program(GRPC_CPP_PLUGIN grpc_cpp_plugin)

add_custom_command(
    OUTPUT "freeswitch_api.pb.cc" "freeswitch_api.pb.h" "freeswitch_api.grpc.pb.cc" "freeswitch_api.grpc.pb.h"
    COMMAND ${Protobuf_PROTOC_EXECUTABLE} 
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
    gRPC::grpc++
    gRPC::grpc++_reflection  # Add this line
    ${Protobuf_LIBRARIES}
    ${CMAKE_THREAD_LIBS_INIT}
)

# 6. Prevent 'lib' prefix (FS expects mod_grpc_api.so, not libmod_grpc_api.so)
set_target_properties(mod_grpc_api PROPERTIES PREFIX "")

# 7. THE FIX: Define the 'install' target
install(TARGETS mod_grpc_api DESTINATION /usr/local/freeswitch/mod)