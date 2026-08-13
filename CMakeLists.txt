cmake_minimum_required(VERSION 3.16)

project(mod_grpc_api VERSION 1.1.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)
find_package(Protobuf REQUIRED)
pkg_check_modules(GRPC REQUIRED IMPORTED_TARGET grpc++)

find_program(PROTOC_EXECUTABLE NAMES protoc)
if(NOT PROTOC_EXECUTABLE)
    message(FATAL_ERROR "protoc was not found. Install protobuf-compiler.")
endif()

find_program(GRPC_CPP_PLUGIN NAMES grpc_cpp_plugin)
if(NOT GRPC_CPP_PLUGIN)
    message(FATAL_ERROR
        "grpc_cpp_plugin was not found. Install protobuf-compiler-grpc or gRPC C++.")
endif()

set(FREESWITCH_ROOT "/usr/local/freeswitch" CACHE PATH
    "FreeSWITCH installation root")
set(FREESWITCH_INCLUDE_DIR "${FREESWITCH_ROOT}/include/freeswitch" CACHE PATH
    "Directory containing switch.h")
set(FREESWITCH_MODULE_DIR "${FREESWITCH_ROOT}/mod" CACHE PATH
    "FreeSWITCH module installation directory")
set(FREESWITCH_CONFIG_DIR "${FREESWITCH_ROOT}/conf/autoload_configs" CACHE PATH
    "FreeSWITCH autoload configuration directory")

if(NOT EXISTS "${FREESWITCH_INCLUDE_DIR}/switch.h")
    message(FATAL_ERROR
        "switch.h was not found in ${FREESWITCH_INCLUDE_DIR}. "
        "Set -DFREESWITCH_INCLUDE_DIR=/path/to/include/freeswitch.")
endif()

find_library(FREESWITCH_LIBRARY
    NAMES freeswitch
    HINTS
        "${FREESWITCH_ROOT}/lib"
        "${FREESWITCH_ROOT}/lib64"
)

# Reflection is optional. The module will compile without it; enabling
# reflection in grpc_api.conf then produces a clear startup error.
find_library(GRPC_REFLECTION_LIBRARY
    NAMES grpc++_reflection
    HINTS ${GRPC_LIBRARY_DIRS}
)

set(PROTO_FILE "${CMAKE_CURRENT_SOURCE_DIR}/freeswitch_api.proto")
set(GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(PROTO_SRCS
    "${GENERATED_DIR}/freeswitch_api.pb.cc"
    "${GENERATED_DIR}/freeswitch_api.grpc.pb.cc"
)
set(PROTO_HDRS
    "${GENERATED_DIR}/freeswitch_api.pb.h"
    "${GENERATED_DIR}/freeswitch_api.grpc.pb.h"
)

add_custom_command(
    OUTPUT ${PROTO_SRCS} ${PROTO_HDRS}
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${GENERATED_DIR}"
    COMMAND "${PROTOC_EXECUTABLE}"
        "--proto_path=${CMAKE_CURRENT_SOURCE_DIR}"
        "--cpp_out=${GENERATED_DIR}"
        "--grpc_out=${GENERATED_DIR}"
        "--plugin=protoc-gen-grpc=${GRPC_CPP_PLUGIN}"
        "${PROTO_FILE}"
    DEPENDS "${PROTO_FILE}"
    COMMENT "Generating protobuf and gRPC C++ sources"
    VERBATIM
)

add_library(mod_grpc_api SHARED
    mod_grpc_api.cpp
    ${PROTO_SRCS}
    ${PROTO_HDRS}
)

set_target_properties(mod_grpc_api PROPERTIES
    PREFIX ""
    OUTPUT_NAME "mod_grpc_api"
    POSITION_INDEPENDENT_CODE ON
)

target_include_directories(mod_grpc_api PRIVATE
    "${FREESWITCH_INCLUDE_DIR}"
    "${GENERATED_DIR}"
    ${Protobuf_INCLUDE_DIRS}
)

target_link_libraries(mod_grpc_api PRIVATE
    PkgConfig::GRPC
    Threads::Threads
    ${CMAKE_DL_LIBS}
)

if(TARGET protobuf::libprotobuf)
    target_link_libraries(mod_grpc_api PRIVATE protobuf::libprotobuf)
else()
    target_link_libraries(mod_grpc_api PRIVATE ${Protobuf_LIBRARIES})
endif()

if(GRPC_REFLECTION_LIBRARY)
    target_compile_definitions(mod_grpc_api PRIVATE
        MOD_GRPC_API_HAVE_REFLECTION=1)
    target_link_libraries(mod_grpc_api PRIVATE "${GRPC_REFLECTION_LIBRARY}")
    message(STATUS "gRPC reflection support: enabled")
else()
    message(STATUS "gRPC reflection support: not available")
endif()

if(FREESWITCH_LIBRARY)
    target_link_libraries(mod_grpc_api PRIVATE "${FREESWITCH_LIBRARY}")
    message(STATUS "Linking FreeSWITCH library: ${FREESWITCH_LIBRARY}")
else()
    message(STATUS
        "libfreeswitch was not found; core symbols will be resolved when "
        "FreeSWITCH loads the module")
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(mod_grpc_api PRIVATE
        -Wall
        -Wextra
        -Wpedantic
    )
endif()

install(TARGETS mod_grpc_api
    LIBRARY DESTINATION "${FREESWITCH_MODULE_DIR}"
    RUNTIME DESTINATION "${FREESWITCH_MODULE_DIR}"
)

install(FILES
    "${CMAKE_CURRENT_SOURCE_DIR}/conf/autoload_configs/grpc_api.conf.xml"
    DESTINATION "${FREESWITCH_CONFIG_DIR}"
)

message(STATUS "FreeSWITCH include directory: ${FREESWITCH_INCLUDE_DIR}")
message(STATUS "Module installation directory: ${FREESWITCH_MODULE_DIR}")
message(STATUS "Configuration directory: ${FREESWITCH_CONFIG_DIR}")
