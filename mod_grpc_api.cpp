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
#include <chrono>
#include <functional>

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

        /* Lines without tabs are summary/footer text — acceptable in a KV block */
        if (line.find('\t') == std::string::npos) continue;

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
    if (cmd != "show") return false;
    /* Reject if args already contain a format specifier:
     * " as " anywhere, or "as " / "as" at the very start */
    if (args.find(" as ") != std::string::npos) return false;
    if (args.size() >= 3 && args.compare(0, 3, "as ") == 0) return false;
    if (args == "as") return false;
    return true;
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
    /* Fix: next_content_line returns lines.size() when nothing is found;
     * adding 1 would wrap the unsigned value past 0, skipping all rows. */
    size_t sep_idx = next_content_line(lines, (size_t)hdr + 1);
    for (size_t i = (sep_idx < lines.size() ? sep_idx + 1 : lines.size());
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
                const char *key = f->string ? f->string : "";
                if (!*key) continue;
                /* Fix: non-string types (number, bool) were silently dropped
                 * as empty string because valuestring is NULL for them.      */
                std::string val;
                switch (f->type & 0xFF) {
                    case cJSON_String:
                        val = f->valuestring ? f->valuestring : "";
                        break;
                    case cJSON_Number: {
                        char nbuf[64];
                        snprintf(nbuf, sizeof(nbuf), "%g", f->valuedouble);
                        val = nbuf;
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
                (*row->mutable_fields())[key] = val;
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
    /* switch_xml_parse_str is destructive (null-terminates tokens in-place).
     * Casting away const on c_str() and letting it write into the std::string
     * buffer is UB.  Copy first.                                             */
    std::string buf = raw;
    switch_xml_t root = switch_xml_parse_str(buf.data(), buf.size());
    if (!root) return;

    if (!root->child) {
        /* Root has no child elements — extract root-level attributes and text
         * so the caller can distinguish a valid (but childless) XML response
         * from an unrecognised shape.                                        */
        Row *row = td->add_rows();
        if (root->attr) {
            for (int i = 0; root->attr[i]; i += 2)
                if (root->attr[i + 1])
                    (*row->mutable_fields())[root->attr[i]] = root->attr[i + 1];
        }
        if (root->txt && root->txt[0])
            (*row->mutable_fields())["value"] = root->txt;
        if (row->fields_size() == 0)
            td->clear_rows();   /* truly empty — no attributes or text */
        else
            td->set_count(1);
        switch_xml_free(root);
        return;
    }

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
 *  THREAD POOL
 *  Fixed-size worker pool backed by FreeSWITCH/APR threading.
 *
 *  switch_queue_t  — APR thread-safe bounded queue (switch_queue_*).
 *  switch_thread_t — APR-managed OS thread (switch_thread_create).
 *  Tasks are heap-allocated std::function<void()>* stored as void*.
 *  switch_queue_term() unblocks all waiting workers on shutdown.
 * ═══════════════════════════════════════════════════════════════ */
class ThreadPool
{
public:
    ThreadPool(size_t n, switch_memory_pool_t *pool) : pool_(pool)
    {
        /* Capacity well above max_concurrent_requests; queue never fills
         * under normal operation because tasks are dispatched one per RPC. */
        switch_queue_create(&queue_, 65535, pool_);

        workers_.resize(n, nullptr);
        for (size_t i = 0; i < n; i++) {
            switch_threadattr_t *attr = nullptr;
            switch_threadattr_create(&attr, pool_);
            switch_threadattr_detach_set(attr, 0); /* joinable */
            switch_thread_create(&workers_[i], attr, worker_func, this, pool_);
        }
    }

    ~ThreadPool()
    {
        /* Bug fix: switch_queue_term() makes switch_queue_pop() return
         * SWITCH_STATUS_GENERR immediately — even if items remain in the
         * queue.  Drain any pending tasks first so their heap memory is
         * freed.  Workers executing concurrently are unaffected.        */
        void *item = nullptr;
        while (switch_queue_trypop(queue_, &item) == SWITCH_STATUS_SUCCESS) {
            delete static_cast<std::function<void()> *>(item);
        }
        /* Now signal workers to stop and wait for them */
        switch_queue_term(queue_);
        for (auto t : workers_) {
            switch_status_t st;
            switch_thread_join(&st, t);
        }
    }

    /* Returns false if the queue rejected the task (terminated or full).
     * Caller must handle failure — task is NOT enqueued on false.       */
    bool post(std::function<void()> f)
    {
        auto *task = new std::function<void()>(std::move(f));
        if (switch_queue_push(queue_, task) != SWITCH_STATUS_SUCCESS) {
            delete task;
            return false;
        }
        return true;
    }

private:
    static void *SWITCH_THREAD_FUNC worker_func(switch_thread_t * /*thread*/,
                                                void             *obj)
    {
        auto *self = static_cast<ThreadPool *>(obj);
        for (;;) {
            void *item = nullptr;
            if (switch_queue_pop(self->queue_, &item) != SWITCH_STATUS_SUCCESS)
                break; /* queue terminated — shut down */
            if (!item) break;
            auto *task = static_cast<std::function<void()> *>(item);
            (*task)();
            delete task;
        }
        return nullptr;
    }

    switch_queue_t              *queue_  = nullptr;
    switch_memory_pool_t        *pool_   = nullptr;
    std::vector<switch_thread_t*> workers_;
};

/* ── Global State ─────────────────────────────────────────── */
static struct
{
    std::unique_ptr<Server>                      server;
    std::unique_ptr<grpc::ServerCompletionQueue> cq;
    std::unique_ptr<ThreadPool>                  thread_pool;
    switch_memory_pool_t                        *pool;
    char                                        *listen_address;
    uint32_t                                     max_concurrent_requests; /* 0 = unlimited */
    int                                          worker_threads;
} globals;

/* Concurrency gate */
static switch_mutex_t *g_concurrency_mutex = nullptr;
static uint32_t        g_active_requests    = 0;
static bool            g_shutting_down      = false;  /* set under g_concurrency_mutex */

/* ═══════════════════════════════════════════════════════════════
 *  COMMAND DENYLIST
 *  Block commands that affect module/server lifecycle or could
 *  lock out the gRPC service itself.
 * ═══════════════════════════════════════════════════════════════ */
static const char *DENIED_COMMANDS[] = {
    "fsctl", "load", "unload", "reload", nullptr
};

static bool is_denied_command(const std::string &cmd)
{
    std::string lower = cmd;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    for (int i = 0; DENIED_COMMANDS[i]; i++)
        if (lower == DENIED_COMMANDS[i]) return true;
    return false;
}

/* ─── Config value parsers ──────────────────────────────────── */
static bool parse_uint_cfg(const char *val, const char *param, uint32_t *out)
{
    char *end = nullptr;
    long  v   = strtol(val, &end, 10);
    if (!end || *end != '\0' || v < 0) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
                          "gRPC API: invalid value for '%s': '%s' "
                          "(expected non-negative integer)\n", param, val);
        return false;
    }
    *out = (uint32_t)v;
    return true;
}

static bool parse_int_cfg(const char *val, const char *param, int *out)
{
    char *end = nullptr;
    long  v   = strtol(val, &end, 10);
    if (!end || *end != '\0') {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
                          "gRPC API: invalid value for '%s': '%s' "
                          "(expected integer)\n", param, val);
        return false;
    }
    *out = (int)v;
    return true;
}

/* ═══════════════════════════════════════════════════════════════
 *  CONFIG RELOAD — applies dynamic parameters at runtime.
 *  listen-address, TLS, and gRPC thread-pool options require a
 *  full module reload (unload + load) to take effect.
 * ═══════════════════════════════════════════════════════════════ */
static void do_config_reload(void)
{
    switch_xml_t cfg, xml, settings, param;
    if ((xml = switch_xml_open_cfg("grpc_api.conf", &cfg, NULL))) {
        if ((settings = switch_xml_child(cfg, "settings"))) {
            for (param  = switch_xml_child(settings, "param");
                 param; param = param->next) {
                const char *var = switch_xml_attr_soft(param, "name");
                const char *val = switch_xml_attr_soft(param, "value");
                if (!strcmp(var, "max-concurrent-requests")) {
                    parse_uint_cfg(val, var, &globals.max_concurrent_requests);
                }
                /* Note: listen-address, tls-*, worker-threads require a
                 * module reload (unload + load) to take effect.             */
            }
        }
        switch_xml_free(xml);
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE,
                          "gRPC API: config reloaded — max_concurrent=%u\n",
                          globals.max_concurrent_requests);
    } else {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
                          "gRPC API: could not open grpc_api.conf for reload\n");
    }
}

/* ─── FreeSWITCH API command: grpc_api reload|status ─────────── */
SWITCH_STANDARD_API(grpc_api_cmd)
{
    if (zstr(cmd) || !strcasecmp(cmd, "help")) {
        stream->write_function(stream,
            "-ERR Usage: grpc_api <reload|status>\n"
            "  reload — re-read grpc_api.conf (max-concurrent-requests only;\n"
            "           other params need module reload)\n"
            "  status — show current runtime values\n");
        return SWITCH_STATUS_SUCCESS;
    }
    if (!strcasecmp(cmd, "reload")) {
        do_config_reload();
        stream->write_function(stream, "+OK Config reloaded\n");
    } else if (!strcasecmp(cmd, "status")) {
        uint32_t active = 0;
        if (g_concurrency_mutex) {
            switch_mutex_lock(g_concurrency_mutex);
            active = g_active_requests;
            switch_mutex_unlock(g_concurrency_mutex);
        }
        stream->write_function(stream,
            "+OK listen=%s max_concurrent=%u worker_threads=%d "
            "active_requests=%u shutting_down=%s\n",
            globals.listen_address          ? globals.listen_address : "(none)",
            globals.max_concurrent_requests,
            globals.worker_threads,
            active,
            g_shutting_down                 ? "true" : "false");
    } else {
        stream->write_function(stream, "-ERR Unknown command: %s\n", cmd);
    }
    return SWITCH_STATUS_SUCCESS;
}

/* ═══════════════════════════════════════════════════════════════
 *  CALL DATA
 *  One heap-allocated instance per in-flight RPC.  The pointer
 *  is used as the completion-queue tag so the polling loop can
 *  find it again after each async operation completes.
 *
 *  State machine:
 *    READY      — RequestExecute() posted; waiting for an
 *                 incoming RPC from the client.
 *    PROCESSING — RPC received; dispatched to worker thread.
 *    FINISH     — Finish() called; waiting for the response
 *                 send to complete (tag fires one final time).
 * ═══════════════════════════════════════════════════════════════ */
struct CallData
{
    fsgrpc::FreeSwitchApi::AsyncService          *service;
    grpc::ServerCompletionQueue                  *cq;
    grpc::ServerContext                           ctx;
    ApiRequest                                    request;
    ApiResponse                                   response;
    grpc::ServerAsyncResponseWriter<ApiResponse>  responder;
    enum State { READY, PROCESSING, FINISH }      state;
    bool                                          counted; /* g_active_requests was incremented */

    CallData(fsgrpc::FreeSwitchApi::AsyncService *svc,
             grpc::ServerCompletionQueue         *cq_)
        : service(svc), cq(cq_), responder(&ctx),
          state(READY), counted(false)
    {
        service->RequestExecute(&ctx, &request, &responder, cq, cq, this);
    }
};

/* ── 2. Module Interface ────────────────────────────────────── */
SWITCH_MODULE_LOAD_FUNCTION(mod_grpc_api_load);
SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_grpc_api_shutdown);
SWITCH_MODULE_RUNTIME_FUNCTION(mod_grpc_api_runtime);
SWITCH_MODULE_DEFINITION(mod_grpc_api, mod_grpc_api_load,
                         mod_grpc_api_shutdown, mod_grpc_api_runtime);

/* ── 4. Load ────────────────────────────────────────────────── */
SWITCH_MODULE_LOAD_FUNCTION(mod_grpc_api_load)
{
    /* Do NOT memset globals — it contains a std::unique_ptr whose constructor
     * has already run; memset-ting it is undefined behaviour.               */
    globals.pool                    = pool;
    globals.listen_address          = switch_core_strdup(pool, "[::]:50051");
    globals.max_concurrent_requests = 0;   /* 0 = unlimited */
    globals.worker_threads          = 4;

    g_active_requests = 0;
    g_shutting_down   = false;
    switch_mutex_init(&g_concurrency_mutex, SWITCH_MUTEX_NESTED, pool);

    *module_interface =
        switch_loadable_module_create_module_interface(pool, modname);

    switch_api_interface_t *api_interface = NULL;
    SWITCH_ADD_API(api_interface, "grpc_api",
                   "gRPC API Module control",
                   grpc_api_cmd, "[reload|status]");

    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE,
                      "gRPC API Module (Typed JSON) Loaded\n");
    return SWITCH_STATUS_SUCCESS;
}

/* ── 5. Runtime ─────────────────────────────────────────────── */
SWITCH_MODULE_RUNTIME_FUNCTION(mod_grpc_api_runtime)
{
    switch_xml_t cfg, xml, settings, param;

    if (!(xml = switch_xml_open_cfg("grpc_api.conf", &cfg, NULL))) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
                          "gRPC API: could not open grpc_api.conf — "
                          "module will not start\n");
        return SWITCH_STATUS_TERM;
    }
    if ((settings = switch_xml_child(cfg, "settings"))) {
        for (param  = switch_xml_child(settings, "param");
             param; param = param->next) {
            const char *var = switch_xml_attr_soft(param, "name");
            const char *val = switch_xml_attr_soft(param, "value");
            if (!strcmp(var, "listen-address")) {
                globals.listen_address =
                    switch_core_strdup(globals.pool, val);
            } else if (!strcmp(var, "max-concurrent-requests")) {
                parse_uint_cfg(val, var, &globals.max_concurrent_requests);
            } else if (!strcmp(var, "worker-threads")) {
                parse_int_cfg(val, var, &globals.worker_threads);
            }
        }
    }
    switch_xml_free(xml);

    if (globals.worker_threads < 1) globals.worker_threads = 1;

    fsgrpc::FreeSwitchApi::AsyncService service;
    ServerBuilder                       builder;

    grpc::reflection::InitProtoReflectionServerBuilderPlugin();

    builder.AddListeningPort(globals.listen_address,
                             grpc::InsecureServerCredentials());
    builder.RegisterService(&service);
    globals.cq = builder.AddCompletionQueue();

    globals.server = builder.BuildAndStart();
    if (!globals.server) {
        globals.cq.reset();
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
                          "Failed to start gRPC Server on %s\n",
                          globals.listen_address);
        return SWITCH_STATUS_TERM;
    }

    globals.thread_pool =
        std::unique_ptr<ThreadPool>(new ThreadPool((size_t)globals.worker_threads,
                                                    globals.pool));

    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_CONSOLE,
                      "gRPC Server (Async) on %s — %d worker thread(s)%s\n",
                      globals.listen_address,
                      globals.worker_threads,
                      (globals.max_concurrent_requests > 0 ?
                          " [concurrency-limited]" : ""));

    /* Seed one pending call slot so the server accepts the first RPC */
    new CallData(&service, globals.cq.get());

    /* ── Completion-queue polling loop ─────────────────────── */
    void *tag;
    bool  ok;
    while (globals.cq->Next(&tag, &ok)) {
        auto *call = static_cast<CallData *>(tag);

        if (call->state == CallData::FINISH) {
            /* Response has been sent (or server is shutting down) */
            if (call->counted && globals.max_concurrent_requests > 0) {
                switch_mutex_lock(g_concurrency_mutex);
                if (g_active_requests > 0) g_active_requests--;
                switch_mutex_unlock(g_concurrency_mutex);
            }
            delete call;
            continue;
        }

        /* state == READY: an incoming RPC has arrived */
        if (!ok) {
            /* Server is shutting down — no RPC data, just clean up */
            delete call;
            continue;
        }

        /* Register a new slot for the next incoming RPC before processing this one */
        new CallData(&service, globals.cq.get());
        call->state = CallData::PROCESSING;

        /* Concurrency gate — reject immediately (no blocking in async mode) */
        if (globals.max_concurrent_requests > 0) {
            switch_mutex_lock(g_concurrency_mutex);
            bool over = g_shutting_down ||
                        g_active_requests >= globals.max_concurrent_requests;
            if (!over) {
                g_active_requests++;
                call->counted = true;
            }
            switch_mutex_unlock(g_concurrency_mutex);

            if (over) {
                const char *msg = g_shutting_down
                    ? "-ERR Server shutting down"
                    : "-ERR Server busy";
                call->response.set_success(false);
                call->response.set_message(std::string(msg) + "\n");
                call->response.set_format("plain");
                fill_plain(msg, false, call->response.mutable_plain());
                call->state = CallData::FINISH;
                call->responder.Finish(call->response, Status::OK, call);
                continue;
            }
        }

        /* Dispatch FreeSWITCH command execution to a worker thread */
        if (!globals.thread_pool->post([call]() {
            std::string cmd  = str_trim(call->request.command());
            std::string args = call->request.arguments();

            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG,
                              "gRPC API: command=%s args=%s\n",
                              cmd.c_str(), args.c_str());

            if (is_denied_command(cmd)) {
                call->response.set_success(false);
                call->response.set_message(
                    "-ERR Command not permitted via gRPC API\n");
                call->response.set_format("plain");
                fill_plain("-ERR Command not permitted via gRPC API",
                           false, call->response.mutable_plain());
            } else {
                ApiExecutionResult result;
                bool               ok_cmd;

                if (should_try_json_variant(cmd, args)) {
                    std::string jargs =
                        args.empty() ? "as json" : args + " as json";
                    result = execute_api_command(cmd, jargs);
                    if (result.status == SWITCH_STATUS_SUCCESS &&
                        detect_format(result.content_type, result.raw) == "json") {
                        ok_cmd = true;
                    } else {
                        result  = execute_api_command(cmd, args);
                        ok_cmd  = (result.status == SWITCH_STATUS_SUCCESS);
                    }
                } else {
                    result = execute_api_command(cmd, args);
                    ok_cmd = (result.status == SWITCH_STATUS_SUCCESS);
                }

                call->response.set_success(ok_cmd);
                call->response.set_message(result.raw);
                route_response(result.content_type, result.raw,
                               ok_cmd, &call->response);
            }

            call->state = CallData::FINISH;
            call->responder.Finish(call->response, Status::OK, call);
        })) {
            /* Queue rejected the task — respond with error immediately */
            if (call->counted && globals.max_concurrent_requests > 0) {
                switch_mutex_lock(g_concurrency_mutex);
                if (g_active_requests > 0) g_active_requests--;
                switch_mutex_unlock(g_concurrency_mutex);
            }
            call->response.set_success(false);
            call->response.set_message("-ERR Internal queue error\n");
            call->response.set_format("plain");
            fill_plain("-ERR Internal queue error", false,
                       call->response.mutable_plain());
            call->state = CallData::FINISH;
            call->responder.Finish(call->response, Status::OK, call);
        }
    }

    /* CQ fully drained — destroy thread pool (joins all worker threads) */
    globals.thread_pool.reset();
    return SWITCH_STATUS_TERM;
}

/* ── 6. Shutdown ────────────────────────────────────────────── */
SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_grpc_api_shutdown)
{
    /* Mark shutdown under mutex so in-flight concurrency checks see it */
    if (g_concurrency_mutex) {
        switch_mutex_lock(g_concurrency_mutex);
        g_shutting_down = true;
        switch_mutex_unlock(g_concurrency_mutex);
    }

    /* Graceful server shutdown — gives in-flight RPCs up to 2 s to finish */
    if (globals.server)
        globals.server->Shutdown(
            std::chrono::system_clock::now() + std::chrono::seconds(2));

    /* Shutdown the completion queue.  This causes the runtime's cq->Next()
     * polling loop to drain all remaining tags and eventually return false,
     * at which point the runtime thread destroys the thread pool and exits. */
    if (globals.cq)
        globals.cq->Shutdown();

    return SWITCH_STATUS_SUCCESS;
}