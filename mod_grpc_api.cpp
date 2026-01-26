/*
 * mod_grpc_api.cpp -- gRPC Server Module for FreeSWITCH
 * Copyright (c) 2026 Krishna Kumar
 * Licensed under the MIT License.
 */

#include <switch.h>
#include <grpcpp/grpcpp.h>
/* Add the reflection header */
#include <grpcpp/ext/proto_server_reflection_plugin.h>
#include "freeswitch_api.grpc.pb.h"

/* Namespaces for clarity */
using fsgrpc::ApiRequest;
using fsgrpc::ApiResponse;
using fsgrpc::FreeSwitchApi;
using grpc::Server;
using grpc::ServerBuilder;
using grpc::ServerContext;
using grpc::Status;

/* 1. Service Implementation */
class ApiServiceImpl final : public FreeSwitchApi::Service
{
    Status Execute(ServerContext *context, const ApiRequest *request, ApiResponse *response) override
    {
        switch_stream_handle_t stream = {0};
        SWITCH_STANDARD_STREAM(stream);

        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "gRPC API: command: %s, arguments: %s\n", request->command().c_str(), request->arguments().c_str());
        // Execute the FreeSWITCH API command internally
        switch_status_t status = switch_api_execute(
            request->command().c_str(),
            request->arguments().c_str(),
            NULL,
            &stream);

        // Populate the response
        response->set_success(status == SWITCH_STATUS_SUCCESS);
        response->set_message(stream.data ? (char *)stream.data : "");

        switch_safe_free(stream.data);
        return Status::OK;
    }
};

/* 2. Global State */
static struct
{
    std::unique_ptr<Server> server;
    switch_memory_pool_t *pool;
    char *listen_address;
} globals;

/* 3. Module Interface Definitions */
SWITCH_MODULE_LOAD_FUNCTION(mod_grpc_api_load);
SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_grpc_api_shutdown);
SWITCH_MODULE_RUNTIME_FUNCTION(mod_grpc_api_runtime);
SWITCH_MODULE_DEFINITION(mod_grpc_api, mod_grpc_api_load, mod_grpc_api_shutdown, mod_grpc_api_runtime);

/* 4. Load Function */
SWITCH_MODULE_LOAD_FUNCTION(mod_grpc_api_load)
{
    memset(&globals, 0, sizeof(globals));
    globals.pool = pool;
    globals.listen_address = switch_core_strdup(globals.pool, "[::]:50051");

    *module_interface = switch_loadable_module_create_module_interface(pool, modname);

    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "gRPC API Module with Reflection Loaded\n");
    return SWITCH_STATUS_SUCCESS;
}

/* 5. Runtime Function */
SWITCH_MODULE_RUNTIME_FUNCTION(mod_grpc_api_runtime)
{
    switch_xml_t cfg, xml, settings, param;

    // Load XML Configuration
    if ((xml = switch_xml_open_cfg("grpc_api.conf", &cfg, NULL)))
    {
        if ((settings = switch_xml_child(cfg, "settings")))
        {
            for (param = switch_xml_child(settings, "param"); param; param = param->next)
            {
                const char *var = switch_xml_attr_soft(param, "name");
                const char *val = switch_xml_attr_soft(param, "value");
                if (!strcmp(var, "listen-address"))
                {
                    globals.listen_address = switch_core_strdup(globals.pool, val);
                }
            }
        }
        switch_xml_free(xml);
    }

    // Initialize gRPC Server
    ApiServiceImpl service;
    ServerBuilder builder;

    // !!! ENABLE REFLECTION HERE !!!
    grpc::reflection::InitProtoReflectionServerBuilderPlugin();

    builder.AddListeningPort(globals.listen_address, grpc::InsecureServerCredentials());
    builder.RegisterService(&service);

    globals.server = builder.BuildAndStart();

    if (!globals.server)
    {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Failed to start gRPC Server on %s\n", globals.listen_address);
        return SWITCH_STATUS_TERM;
    }

    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_CONSOLE, "gRPC Server (Reflection Enabled) on %s\n", globals.listen_address);

    globals.server->Wait();
    return SWITCH_STATUS_TERM;
}

/* 6. Shutdown Function */
SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_grpc_api_shutdown)
{
    if (globals.server)
    {
        globals.server->Shutdown(std::chrono::system_clock::now() + std::chrono::seconds(2));
    }
    return SWITCH_STATUS_SUCCESS;
}