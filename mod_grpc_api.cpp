/*
 * mod_grpc_api.cpp -- gRPC Server Module for FreeSWITCH
 * Copyright (c) 2026 Krishna Kumar
 * Licensed under the MIT License.
 */

#include <switch.h>
#include <switch_json.h>
#include <grpcpp/grpcpp.h>
#include <grpcpp/ext/proto_server_reflection_plugin.h>
#include "freeswitch_api.grpc.pb.h"

#include <string>
#include <sstream>
#include <vector>
#include <algorithm>

using fsgrpc::ApiRequest;
using fsgrpc::ApiResponse;
using fsgrpc::TableData;
using fsgrpc::JsonData;
using fsgrpc::PlainData;
using fsgrpc::Row;
using fsgrpc::FreeSwitchApi;
using grpc::Server;
using grpc::ServerBuilder;
using grpc::ServerContext;
using grpc::Status;

/* ═══════════════════════════════════════════════════════════════
 *  STRING UTILITIES
 * ═══════════════════════════════════════════════════════════════ */
static std::string str_trim(const std::string &s)
{
    const char *ws = " \t\r\n";
    size_t a = s.find_first_not_of(ws);
    if (a == std::string::npos) return "";
    return s.substr(a, s.find_last_not_of(ws) - a + 1);
}

static std::vector<std::string> split_lines(const std::string &s)
{
    std::vector<std::string> out;
    std::istringstream ss(s);
    std::string line;
    while (std::getline(ss, line)) out.push_back(line);
    return out;
}

static std::vector<std::string> split_tab(const std::string &s)
{
    std::vector<std::string> out;
    std::istringstream ss(s);
    std::string tok;
    while (std::getline(ss, tok, '\t')) out.push_back(tok);
    return out;
}

static bool is_separator(const std::string &l)
{
    return !l.empty() &&
           l.find_first_not_of("=+-| \t\r\n") == std::string::npos;
}

static size_t next_content_line(const std::vector<std::string> &lines,
                                size_t                          start)
{
    for (size_t i = start; i < lines.size(); i++)
        if (!str_trim(lines[i]).empty()) return i;
    return lines.size();
}

static bool looks_like_key_value_block(const std::vector<std::string> &lines)
{
    size_t key_value_lines = 0;

    for (const auto &line : lines) {
        std::string trimmed = str_trim(line);
        if (trimmed.empty() || is_separator(line)) continue;

        auto cols = split_tab(line);
        if (cols.size() != 2) return false;
        if (str_trim(cols[0]).empty()) return false;

        key_value_lines++;
    }

    return key_value_lines > 0;
}

static bool fill_key_value_table(const std::vector<std::string> &lines,
                                 TableData                      *td)
{
    Row         *row            = td->add_rows();
    std::string  summary;
    bool         saw_key_value  = false;

    for (const auto &line : lines) {
        std::string trimmed = str_trim(line);
        if (trimmed.empty() || is_separator(line)) continue;

        if (line.find('\t') == std::string::npos) {
            summary += (summary.empty() ? "" : " | ") + trimmed;
            continue;
        }

        auto cols = split_tab(line);
        if (cols.size() != 2) {
            td->clear_rows();
            td->set_summary("");
            td->set_count(0);
            return false;
        }

        std::string key   = str_trim(cols[0]);
        std::string value = str_trim(cols[1]);
        if (key.empty()) {
            td->clear_rows();
            td->set_summary("");
            td->set_count(0);
            return false;
        }

        (*row->mutable_fields())[key] = value;
        saw_key_value = true;
    }

    if (!saw_key_value) {
        td->clear_rows();
        return false;
    }

    td->set_summary(summary);
    td->set_count((uint32_t)td->rows_size());
    return true;
}

struct ApiExecutionResult {
    switch_status_t status = SWITCH_STATUS_FALSE;
    std::string     raw;
    std::string     content_type;
};

static ApiExecutionResult execute_api_command(const std::string &cmd,
                                              const std::string &args)
{
    ApiExecutionResult     result;
    switch_stream_handle_t stream = {0};

    SWITCH_STANDARD_STREAM(stream);
    switch_event_create(&stream.param_event, SWITCH_EVENT_REQUEST_PARAMS);

    result.status = switch_api_execute(cmd.c_str(), args.c_str(), NULL, &stream);
    result.raw = stream.data ? static_cast<char *>(stream.data) : "";

    if (stream.param_event) {
        const char *ct = switch_event_get_header(stream.param_event, "Content-Type");
        if (ct) result.content_type = ct;
        switch_event_destroy(&stream.param_event);
    }

    switch_safe_free(stream.data);
    return result;
}

static bool should_try_json_variant(const std::string &cmd,
                                    const std::string &args)
{
    return cmd == "show" && args.find(" as ") == std::string::npos;
}

/* ═══════════════════════════════════════════════════════════════
 *  FORMAT DETECTION
 *  1. Content-Type in param_event (set by the command itself)
 *  2. Sniff first byte of buffer
 * ═══════════════════════════════════════════════════════════════ */
static std::string detect_format(const std::string &content_type,
                                 const std::string &raw)
{
    if (!content_type.empty()) {
        if (strstr(content_type.c_str(), "json")) return "json";
        if (strstr(content_type.c_str(), "xml"))  return "xml";
    }
    size_t p = raw.find_first_not_of(" \t\r\n");
    if (p != std::string::npos) {
        if (raw[p] == '{' || raw[p] == '[') return "json";
        if (raw[p] == '<')                  return "xml";
    }
    return "plain";
}

/* ═══════════════════════════════════════════════════════════════
 *  FILL TableData from tab-separated FS output
 *
 *  FreeSWITCH table format:
 *    Name\tType\tData\tState        <- header row
 *    ============================   <- separator
 *    external\tprofile\t...\tRUN   <- data rows
 *    2 profiles 0 aliases           <- summary footer
 *
 *  Returns true if output looked like a table.
 * ═══════════════════════════════════════════════════════════════ */
static bool fill_table(const std::string &raw, TableData *td)
{
    auto lines = split_lines(raw);

    if (looks_like_key_value_block(lines))
        return fill_key_value_table(lines, td);

    int  hdr   = -1;
    std::vector<std::string> headers;

    for (size_t i = 0; i < lines.size(); i++) {
        if (lines[i].find('\t') != std::string::npos
            && !is_separator(lines[i])) {
            size_t next = next_content_line(lines, i + 1);
            if (next >= lines.size() || !is_separator(lines[next])) continue;

            for (auto &c : split_tab(lines[i])) {
                std::string h = str_trim(c);
                if (!h.empty()) headers.push_back(h);
            }
            if (headers.size() >= 2) { hdr = (int)i; break; }
            headers.clear();
        }
    }
    if (hdr < 0) return fill_key_value_table(lines, td);

    std::string summary;
    for (size_t i = next_content_line(lines, (size_t)hdr + 1) + 1;
         i < lines.size(); i++) {
        const std::string &l = lines[i];
        if (str_trim(l).empty() || is_separator(l)) continue;
        if (l.find('\t') == std::string::npos) {
            summary += (summary.empty() ? "" : " | ") + str_trim(l);
            continue;
        }
        Row *row  = td->add_rows();
        auto cols = split_tab(l);
        size_t n  = std::min(cols.size(), headers.size());
        for (size_t c = 0; c < n; c++)
            (*row->mutable_fields())[headers[c]] = str_trim(cols[c]);
    }
    td->set_summary(summary);
    td->set_count((uint32_t)td->rows_size());
    return true;
}

/* ═══════════════════════════════════════════════════════════════
 *  FILL JsonData from native JSON output
 *
 *  FreeSWITCH "show X as json" returns:
 *    {"row_count":N,"rows":[{"col":"val",...},...]}
 *
 *  IMPORTANT: FreeSWITCH cJSON uses "string" (NOT "name") for
 *  the object key field: char *string
 * ═══════════════════════════════════════════════════════════════ */
static void fill_json(const std::string &raw, JsonData *jd)
{
    cJSON *root = cJSON_Parse(raw.c_str());
    if (!root) {
        jd->set_raw(raw);
        return;
    }

    /* row_count */
    cJSON *rc = cJSON_GetObjectItem(root, "row_count");
    if (rc && (rc->type & 0xFF) == cJSON_Number)
        jd->set_row_count((uint32_t)rc->valuedouble);

    /* rows[] */
    cJSON *rows_arr = cJSON_GetObjectItem(root, "rows");
    if (rows_arr && (rows_arr->type & 0xFF) == cJSON_Array) {
        for (cJSON *item = rows_arr->child; item; item = item->next) {
            if ((item->type & 0xFF) != cJSON_Object) continue;
            Row *row = jd->add_rows();
            /* FreeSWITCH cJSON: key field is "string" not "name" */
            for (cJSON *f = item->child; f; f = f->next) {
                const char *key = f->string      ? f->string      : "";
                const char *val = f->valuestring ? f->valuestring : "";
                if (*key) (*row->mutable_fields())[key] = val;
            }
        }
    } else if ((root->type & 0xFF) == cJSON_Object) {
        /* Flat JSON object → fill fields map */
        for (cJSON *f = root->child; f; f = f->next) {
            /* FreeSWITCH cJSON: key field is "string" not "name" */
            const char *key = f->string ? f->string : "";
            if (!*key) continue;
            std::string val;
            switch (f->type & 0xFF) {
                case cJSON_String:
                    val = f->valuestring ? f->valuestring : "";
                    break;
                case cJSON_Number: {
                    char buf[64];
                    snprintf(buf, sizeof(buf), "%g", f->valuedouble);
                    val = buf;
                    break;
                }
                case cJSON_True:  val = "true";  break;
                case cJSON_False: val = "false"; break;
                default: {
                    char *s = cJSON_PrintUnformatted(f);
                    if (s) { val = s; free(s); }
                    break;
                }
            }
            (*jd->mutable_fields())[key] = val;
        }
    } else {
        /* Array or other at root level */
        char *s = cJSON_PrintUnformatted(root);
        if (s) { jd->set_raw(s); free(s); }
    }

    cJSON_Delete(root);
}

/* ═══════════════════════════════════════════════════════════════
 *  FILL PlainData for simple responses
 *  e.g. "+OK", "-ERR ...", "FreeSWITCH Version 1.10..."
 * ═══════════════════════════════════════════════════════════════ */
static void fill_plain(const std::string &raw, bool ok, PlainData *pd)
{
    pd->set_ok(ok);
    pd->set_response(str_trim(raw));
}

/* ═══════════════════════════════════════════════════════════════
 *  FILL XML -> TableData
 *  Commands like "sofia xmlstatus" return XML — we flatten
 *  top-level child elements into rows.
 * ═══════════════════════════════════════════════════════════════ */
static void fill_xml_as_table(const std::string &raw, TableData *td)
{
    switch_xml_t root = switch_xml_parse_str(
        const_cast<char *>(raw.c_str()), raw.size());
    if (!root) return;

    for (switch_xml_t child = root->child; child; child = child->ordered) {
        Row *row = td->add_rows();
        /* XML attributes → fields */
        if (child->attr) {
            for (int i = 0; child->attr[i]; i += 2)
                if (child->attr[i + 1])
                    (*row->mutable_fields())[child->attr[i]] =
                        child->attr[i + 1];
        }
        /* XML child text nodes → fields */
        for (switch_xml_t sub = child->child; sub; sub = sub->ordered)
            if (sub->name && sub->txt && sub->txt[0])
                (*row->mutable_fields())[sub->name] = sub->txt;
        /* Element text */
        if (child->txt && child->txt[0])
            (*row->mutable_fields())["value"] = child->txt;
    }
    td->set_count((uint32_t)td->rows_size());
    switch_xml_free(root);
}

/* ═══════════════════════════════════════════════════════════════
 *  MASTER ROUTER
 *  Detects format → fills the correct typed field in ApiResponse.
 *
 *  Postman renders proto map/repeated fields as real JSON
 *  objects/arrays — no escaping, no binary garbage.
 * ═══════════════════════════════════════════════════════════════ */
static void route_response(const std::string &content_type,
                           const std::string &raw,
                           bool               ok,
                           ApiResponse       *response)
{
    std::string fmt = detect_format(content_type, raw);
    response->set_format(fmt);

    if (fmt == "json") {
        fill_json(raw, response->mutable_json());

    } else if (fmt == "xml") {
        fill_xml_as_table(raw, response->mutable_table());
        response->set_format("xml");

    } else {
        /* Plain text: try tab-separated table first */
        if (!fill_table(raw, response->mutable_table())) {
            response->clear_table();
            fill_plain(raw, ok, response->mutable_plain());
        } else {
            response->set_format("table");
        }
    }
}

/* ═══════════════════════════════════════════════════════════════
 *  1. SERVICE IMPLEMENTATION
 * ═══════════════════════════════════════════════════════════════ */
class ApiServiceImpl final : public FreeSwitchApi::Service
{
    Status Execute(ServerContext     *context,
                   const ApiRequest  *request,
                   ApiResponse       *response) override
    {
        (void)context;
        std::string cmd  = request->command();
        std::string args = request->arguments();

        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG,
                          "gRPC API: command=%s args=%s\n",
                          cmd.c_str(), args.c_str());

        ApiExecutionResult primary = execute_api_command(cmd, args);
        ApiExecutionResult structured = primary;
        bool               ok = (primary.status == SWITCH_STATUS_SUCCESS);

        response->set_success(ok);
        response->set_message(primary.raw);

        if (ok && should_try_json_variant(cmd, args)) {
            ApiExecutionResult json_variant =
                execute_api_command(cmd, args + " as json");

            if (json_variant.status == SWITCH_STATUS_SUCCESS &&
                detect_format(json_variant.content_type, json_variant.raw) == "json") {
                structured = std::move(json_variant);
            }
        }

        /* Fill the typed structured field — Postman renders this
           as proper nested JSON with no escaping or binary       */
        route_response(structured.content_type, structured.raw, ok, response);
        return Status::OK;
    }
};

/* ── 2. Global State ────────────────────────────────────────── */
static struct
{
    std::unique_ptr<Server> server;
    switch_memory_pool_t   *pool;
    char                   *listen_address;
} globals;

/* ── 3. Module Interface ────────────────────────────────────── */
SWITCH_MODULE_LOAD_FUNCTION(mod_grpc_api_load);
SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_grpc_api_shutdown);
SWITCH_MODULE_RUNTIME_FUNCTION(mod_grpc_api_runtime);
SWITCH_MODULE_DEFINITION(mod_grpc_api, mod_grpc_api_load,
                         mod_grpc_api_shutdown, mod_grpc_api_runtime);

/* ── 4. Load ────────────────────────────────────────────────── */
SWITCH_MODULE_LOAD_FUNCTION(mod_grpc_api_load)
{
    memset(&globals, 0, sizeof(globals));
    globals.pool           = pool;
    globals.listen_address = switch_core_strdup(globals.pool, "[::]:50051");
    *module_interface      =
        switch_loadable_module_create_module_interface(pool, modname);
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE,
                      "gRPC API Module (Typed JSON) Loaded\n");
    return SWITCH_STATUS_SUCCESS;
}

/* ── 5. Runtime ─────────────────────────────────────────────── */
SWITCH_MODULE_RUNTIME_FUNCTION(mod_grpc_api_runtime)
{
    switch_xml_t cfg, xml, settings, param;

    if ((xml = switch_xml_open_cfg("grpc_api.conf", &cfg, NULL))) {
        if ((settings = switch_xml_child(cfg, "settings"))) {
            for (param  = switch_xml_child(settings, "param");
                 param; param = param->next) {
                const char *var = switch_xml_attr_soft(param, "name");
                const char *val = switch_xml_attr_soft(param, "value");
                if (!strcmp(var, "listen-address"))
                    globals.listen_address =
                        switch_core_strdup(globals.pool, val);
            }
        }
        switch_xml_free(xml);
    }

    ApiServiceImpl service;
    ServerBuilder  builder;

    grpc::reflection::InitProtoReflectionServerBuilderPlugin();
    builder.AddListeningPort(globals.listen_address,
                             grpc::InsecureServerCredentials());
    builder.RegisterService(&service);

    globals.server = builder.BuildAndStart();
    if (!globals.server) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
                          "Failed to start gRPC Server on %s\n",
                          globals.listen_address);
        return SWITCH_STATUS_TERM;
    }
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_CONSOLE,
                      "gRPC Server (Typed JSON) on %s\n",
                      globals.listen_address);
    globals.server->Wait();
    return SWITCH_STATUS_TERM;
}

/* ── 6. Shutdown ────────────────────────────────────────────── */
SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_grpc_api_shutdown)
{
    if (globals.server)
        globals.server->Shutdown(
            std::chrono::system_clock::now() + std::chrono::seconds(2));
    return SWITCH_STATUS_SUCCESS;
}
