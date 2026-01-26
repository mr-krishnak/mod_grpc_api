# mod_grpc_api for FreeSWITCH

A high-performance gRPC server module for FreeSWITCH that allows external applications 
to execute API commands over a structured, parallel-friendly interface.

## Features
- **Parallelism:** Built on gRPC's asynchronous/multi-threaded core.
- **Reflection:** Supports gRPC Server Reflection for easy testing with Postman.
- **JSON Support:** Returns structured responses.

## Installation
1. Install dependencies: `libgrpc++-dev`, `protobuf-compiler-grpc`, `libfreeswitch-dev`.
   
   ```
   sudo apt-get install -y libprotobuf-dev protobuf-compiler protobuf-compiler-grpc libgrpc++-dev

2. Build:
   
   ```bash
   mkdir build && cd build
   cmake ..
   make && sudo make install