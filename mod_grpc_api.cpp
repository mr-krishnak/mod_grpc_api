/*
 * mod_grpc_api.cpp -- asynchronous gRPC API module for FreeSWITCH
 *
 * Copyright (c) 2026 Krishna Kumar
 * Licensed under the MIT License.
 */

#include <switch.h>
#include <switch_json.h>

#include <grpcpp/grpcpp.h>
#ifdef MOD_GRPC_API_HAVE_REFLECTION
#include <grpcpp/ext/proto_server_reflection_plugin.h>
#endif

#include "freeswitch_api.grpc.pb.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cctype>
#include <climits>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <exception>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

using fsgrpc::ApiRequest;
using fsgrpc::ApiResponse;
using fsgrpc::FreeSwitchApi;
using fsgrpc::JsonData;
using fsgrpc::PlainData;
using fsgrpc::Row;
using fsgrpc::TableData;

using grpc::Server;
using grpc::ServerBuilder;
using grpc::ServerCompletionQueue;
using grpc::ServerContext;
using grpc::Status;
using grpc::StatusCode;

namespace {

constexpr std::size_t kMaxCommandBytes = 128;
constexpr std::size_t kMaxArgumentBytes = 64U * 1024U;
constexpr std::size_t kMaxLogArgumentBytes = 256;
constexpr std::size_t kMaxStructuredParseBytes = 4U * 1024U * 1024U;
constexpr int kMaxReceiveMessageBytes = 128 * 1024;
constexpr std::size_t kMinResponseBytes = 4U * 1024U;
constexpr std::size_t kMaxResponseBytes = 64U * 1024U * 1024U;

struct ModuleConfig {
    std::string listen_address = "127.0.0.1:50051";
    std::size_t worker_threads = 8;
    std::uint32_t max_concurrent_requests = 64;
    std::size_t queue_capacity = 64;
    std::size_t acceptor_count = 8;
    std::size_t max_response_bytes = 8U * 1024U * 1024U;
    std::uint32_t shutdown_grace_ms = 5000;
    bool enable_reflection = false;
    bool log_arguments = false;
    std::unordered_set<std::string> allowed_commands;
};

static ModuleConfig make_default_config()
{
    ModuleConfig config;
    config.allowed_commands = {
        "bgapi",
        "global_getvar",
        "module_exists",
        "originate",
        "show",
        "status",
        "uuid_*",
        "version"
    };
    return config;
}

class ThreadPool;

struct GlobalState {
    ModuleConfig config = make_default_config();
    std::shared_ptr<Server> server;
    std::shared_ptr<ServerCompletionQueue> cq;
    std::shared_ptr<ThreadPool> thread_pool;
};

GlobalState globals;
std::mutex g_state_mutex;
std::mutex g_lifecycle_mutex;
std::uint32_t g_active_requests = 0;
std::atomic<std::uint32_t> g_background_jobs{0};
std::atomic<bool> g_shutting_down{false};
std::atomic<bool> g_shutdown_sequence_started{false};

static std::string str_trim(const std::string &value)
{
    const char *whitespace = " \t\r\n";
    const std::size_t first = value.find_first_not_of(whitespace);
    if (first == std::string::npos) {
        return "";
    }
    const std::size_t last = value.find_last_not_of(whitespace);
    return value.substr(first, last - first + 1);
}

static std::string to_lower_ascii(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) {
                       return static_cast<char>(std::tolower(c));
                   });
    return value;
}

static std::vector<std::string> split_lines(const std::string &value)
{
    std::vector<std::string> output;
    std::istringstream stream(value);
    std::string line;
    while (std::getline(stream, line)) {
        output.push_back(line);
    }
    return output;
}

static std::vector<std::string> split_tab(const std::string &value)
{
    std::vector<std::string> output;
    std::istringstream stream(value);
    std::string token;
    while (std::getline(stream, token, '\t')) {
        output.push_back(token);
    }
    return output;
}

static std::string sanitize_log_value(const std::string &value)
{
    const std::size_t length = std::min(value.size(), kMaxLogArgumentBytes);
    std::string output = value.substr(0, length);
    for (char &c : output) {
        const unsigned char uc = static_cast<unsigned char>(c);
        if (c == '\r' || c == '\n' || c == '\0' || std::iscntrl(uc)) {
            c = ' ';
        }
    }
    if (value.size() > length) {
        output += "...";
    }
    return output;
}

static bool is_separator(const std::string &line)
{
    return !line.empty() &&
           line.find_first_not_of("=+-| \t\r\n") == std::string::npos;
}

static std::size_t next_content_line(const std::vector<std::string> &lines,
                                     std::size_t start)
{
    for (std::size_t i = start; i < lines.size(); ++i) {
        if (!str_trim(lines[i]).empty()) {
            return i;
        }
    }
    return lines.size();
}

static bool looks_like_key_value_block(const std::vector<std::string> &lines)
{
    std::size_t key_value_lines = 0;
    for (const std::string &line : lines) {
        const std::string trimmed = str_trim(line);
        if (trimmed.empty() || is_separator(line)) {
            continue;
        }
        if (line.find('\t') == std::string::npos) {
            continue;
        }
        const std::vector<std::string> columns = split_tab(line);
        if (columns.size() != 2 || str_trim(columns[0]).empty()) {
            return false;
        }
        ++key_value_lines;
    }
    return key_value_lines > 0;
}

static bool fill_key_value_table(const std::vector<std::string> &lines,
                                 TableData *table)
{
    Row *row = table->add_rows();
    std::string summary;
    bool found = false;

    for (const std::string &line : lines) {
        const std::string trimmed = str_trim(line);
        if (trimmed.empty() || is_separator(line)) {
            continue;
        }
        if (line.find('\t') == std::string::npos) {
            summary += (summary.empty() ? "" : " | ") + trimmed;
            continue;
        }
        const std::vector<std::string> columns = split_tab(line);
        if (columns.size() != 2) {
            table->clear_rows();
            table->set_summary("");
            table->set_count(0);
            return false;
        }
        const std::string key = str_trim(columns[0]);
        if (key.empty()) {
            table->clear_rows();
            table->set_summary("");
            table->set_count(0);
            return false;
        }
        (*row->mutable_fields())[key] = str_trim(columns[1]);
        found = true;
    }

    if (!found) {
        table->clear_rows();
        return false;
    }

    table->set_summary(summary);
    table->set_count(static_cast<std::uint32_t>(table->rows_size()));
    return true;
}

static bool fill_table(const std::string &raw, TableData *table)
{
    const std::vector<std::string> lines = split_lines(raw);

    // Look for an explicit header + separator before testing the key/value
    // shape. Otherwise a valid two-column table is collapsed into one row.
    int header_index = -1;
    std::vector<std::string> headers;

    for (std::size_t i = 0; i < lines.size(); ++i) {
        if (lines[i].find('\t') == std::string::npos || is_separator(lines[i])) {
            continue;
        }
        const std::size_t next = next_content_line(lines, i + 1);
        if (next >= lines.size() || !is_separator(lines[next])) {
            continue;
        }
        for (const std::string &column : split_tab(lines[i])) {
            const std::string header = str_trim(column);
            if (!header.empty()) {
                headers.push_back(header);
            }
        }
        if (headers.size() >= 2) {
            header_index = static_cast<int>(i);
            break;
        }
        headers.clear();
    }

    if (header_index < 0) {
        if (!looks_like_key_value_block(lines)) {
            return false;
        }
        return fill_key_value_table(lines, table);
    }

    const std::size_t separator_index =
        next_content_line(lines, static_cast<std::size_t>(header_index) + 1);
    std::string summary;

    for (std::size_t i = separator_index < lines.size()
                             ? separator_index + 1
                             : lines.size();
         i < lines.size(); ++i) {
        const std::string &line = lines[i];
        if (str_trim(line).empty() || is_separator(line)) {
            continue;
        }
        if (line.find('\t') == std::string::npos) {
            summary += (summary.empty() ? "" : " | ") + str_trim(line);
            continue;
        }

        Row *row = table->add_rows();
        const std::vector<std::string> columns = split_tab(line);
        const std::size_t count = std::min(columns.size(), headers.size());
        for (std::size_t column = 0; column < count; ++column) {
            (*row->mutable_fields())[headers[column]] = str_trim(columns[column]);
        }
    }

    table->set_summary(summary);
    table->set_count(static_cast<std::uint32_t>(table->rows_size()));
    return true;
}

static std::string json_value_as_string(cJSON *item)
{
    if (!item) {
        return "";
    }

    switch (item->type & 0xFF) {
        case cJSON_String:
            return item->valuestring ? item->valuestring : "";
        case cJSON_Number: {
            char buffer[64];
            switch_snprintf(buffer, sizeof(buffer), "%g", item->valuedouble);
            return buffer;
        }
        case cJSON_True:
            return "true";
        case cJSON_False:
            return "false";
        case cJSON_NULL:
            return "null";
        default: {
            char *printed = cJSON_PrintUnformatted(item);
            if (!printed) {
                return "";
            }
            std::string output(printed);
            cJSON_free(printed);
            return output;
        }
    }
}

static void fill_json(const std::string &raw, JsonData *json)
{
    cJSON *root = cJSON_Parse(raw.c_str());
    if (!root) {
        json->set_raw(raw);
        return;
    }

    cJSON *row_count = cJSON_GetObjectItem(root, "row_count");
    if (row_count && (row_count->type & 0xFF) == cJSON_Number &&
        row_count->valuedouble >= 0.0 &&
        row_count->valuedouble <= static_cast<double>(std::numeric_limits<std::uint32_t>::max())) {
        json->set_row_count(static_cast<std::uint32_t>(row_count->valuedouble));
    }

    cJSON *rows = cJSON_GetObjectItem(root, "rows");
    if (rows && (rows->type & 0xFF) == cJSON_Array) {
        for (cJSON *item = rows->child; item; item = item->next) {
            if ((item->type & 0xFF) != cJSON_Object) {
                continue;
            }
            Row *row = json->add_rows();
            for (cJSON *field = item->child; field; field = field->next) {
                const char *key = field->string ? field->string : "";
                if (*key) {
                    (*row->mutable_fields())[key] = json_value_as_string(field);
                }
            }
        }
    } else if ((root->type & 0xFF) == cJSON_Object) {
        for (cJSON *field = root->child; field; field = field->next) {
            const char *key = field->string ? field->string : "";
            if (*key) {
                (*json->mutable_fields())[key] = json_value_as_string(field);
            }
        }
    } else {
        char *printed = cJSON_PrintUnformatted(root);
        if (printed) {
            json->set_raw(printed);
            cJSON_free(printed);
        }
    }

    cJSON_Delete(root);
}

static void fill_plain(const std::string &raw, bool ok, PlainData *plain)
{
    plain->set_response(str_trim(raw));
    plain->set_ok(ok);
}

static void fill_xml_as_table(const std::string &raw, TableData *table)
{
    std::string buffer = raw;
    switch_xml_t root = switch_xml_parse_str(buffer.data(), buffer.size());
    if (!root) {
        return;
    }

    if (!root->child) {
        Row *row = table->add_rows();
        if (root->attr) {
            for (int i = 0; root->attr[i]; i += 2) {
                if (root->attr[i + 1]) {
                    (*row->mutable_fields())[root->attr[i]] = root->attr[i + 1];
                }
            }
        }
        if (root->txt && root->txt[0]) {
            (*row->mutable_fields())["value"] = root->txt;
        }
        if (row->fields_size() == 0) {
            table->clear_rows();
        } else {
            table->set_count(1);
        }
        switch_xml_free(root);
        return;
    }

    for (switch_xml_t child = root->child; child; child = child->ordered) {
        Row *row = table->add_rows();
        if (child->attr) {
            for (int i = 0; child->attr[i]; i += 2) {
                if (child->attr[i + 1]) {
                    (*row->mutable_fields())[child->attr[i]] = child->attr[i + 1];
                }
            }
        }
        for (switch_xml_t sub = child->child; sub; sub = sub->ordered) {
            if (sub->name && sub->txt && sub->txt[0]) {
                (*row->mutable_fields())[sub->name] = sub->txt;
            }
        }
        if (child->txt && child->txt[0]) {
            (*row->mutable_fields())["value"] = child->txt;
        }
    }

    table->set_count(static_cast<std::uint32_t>(table->rows_size()));
    switch_xml_free(root);
}

static std::string detect_format(const std::string &content_type,
                                 const std::string &raw)
{
    const std::string lowered_type = to_lower_ascii(content_type);
    if (lowered_type.find("json") != std::string::npos) {
        return "json";
    }
    if (lowered_type.find("xml") != std::string::npos) {
        return "xml";
    }

    const std::size_t first = raw.find_first_not_of(" \t\r\n");
    if (first != std::string::npos) {
        if (raw[first] == '{' || raw[first] == '[') {
            return "json";
        }
        if (raw[first] == '<') {
            return "xml";
        }
    }
    return "plain";
}

static void route_response(const std::string &content_type,
                           const std::string &raw,
                           bool ok,
                           ApiResponse *response)
{
    const std::string format = detect_format(content_type, raw);
    response->set_format(format);

    if (format == "json") {
        fill_json(raw, response->mutable_json());
        return;
    }

    if (format == "xml") {
        fill_xml_as_table(raw, response->mutable_table());
        return;
    }

    if (!fill_table(raw, response->mutable_table())) {
        response->clear_table();
        fill_plain(raw, ok, response->mutable_plain());
    } else {
        response->set_format("table");
    }
}

struct ApiExecutionResult {
    switch_status_t status = SWITCH_STATUS_FALSE;
    std::string raw;
    std::string content_type;
};

static ApiExecutionResult execute_api_command(const std::string &command,
                                               const std::string &arguments)
{
    ApiExecutionResult result;
    switch_stream_handle_t stream = {};
    SWITCH_STANDARD_STREAM(stream);

    if (switch_event_create(&stream.param_event,
                            SWITCH_EVENT_REQUEST_PARAMS) != SWITCH_STATUS_SUCCESS) {
        stream.param_event = nullptr;
    }

    result.status = switch_api_execute(command.c_str(), arguments.c_str(),
                                       nullptr, &stream);
    result.raw = stream.data ? static_cast<const char *>(stream.data) : "";

    if (stream.param_event) {
        const char *content_type =
            switch_event_get_header(stream.param_event, "Content-Type");
        if (content_type) {
            result.content_type = content_type;
        }
        switch_event_destroy(&stream.param_event);
    }

    switch_safe_free(stream.data);
    return result;
}

static bool api_result_succeeded(const ApiExecutionResult &result)
{
    if (result.status != SWITCH_STATUS_SUCCESS) {
        return false;
    }
    const std::string trimmed = str_trim(result.raw);
    return trimmed.rfind("-ERR", 0) != 0 && trimmed.rfind("-USAGE", 0) != 0;
}

static bool truncate_response(std::string *raw, std::size_t maximum)
{
    if (raw->size() <= maximum) {
        return false;
    }

    const std::string marker = "\n... [mod_grpc_api response truncated]\n";
    if (maximum <= marker.size()) {
        raw->assign(marker.substr(0, maximum));
    } else {
        raw->resize(maximum - marker.size());
        raw->append(marker);
    }
    return true;
}

static std::size_t max_wire_response_bytes(std::size_t raw_limit)
{
    constexpr std::size_t overhead = 1024U * 1024U;
    const std::size_t maximum = static_cast<std::size_t>(INT_MAX);
    if (raw_limit > (maximum - overhead) / 2U) {
        return maximum;
    }
    return raw_limit * 2U + overhead;
}

static bool valid_command_name(const std::string &command)
{
    if (command.empty() || command.size() > kMaxCommandBytes ||
        command.find('\0') != std::string::npos) {
        return false;
    }

    return std::all_of(command.begin(), command.end(), [](unsigned char c) {
        return (c >= 'a' && c <= 'z') ||
               (c >= '0' && c <= '9') ||
               c == '_' || c == '-' || c == '.';
    });
}

static const std::unordered_set<std::string> &hard_denied_commands()
{
    static const std::unordered_set<std::string> denied = {
        "bg_spawn",
        "bg_system",
        "eval",
        "expand",
        "fsctl",
        "load",
        "reload",
        "reloadxml",
        "sched_api",
        "sched_del",
        "sched_transfer",
        "shutdown",
        "spawn",
        "spawn_stream",
        "system",
        "unload",
        "unsched_api"
    };
    return denied;
}

static bool command_matches_allowlist_rule(const std::string &command,
                                           const std::string &rule)
{
    if (rule == "uuid_*") {
        return command.size() > 5 && command.rfind("uuid_", 0) == 0;
    }
    return command == rule;
}

static bool configured_command_allowed(const ModuleConfig &config,
                                       const std::string &command)
{
    return std::any_of(
        config.allowed_commands.begin(), config.allowed_commands.end(),
        [&command](const std::string &rule) {
            return command_matches_allowlist_rule(command, rule);
        });
}

static bool command_allowed(const std::string &command,
                            std::size_t *max_response_bytes,
                            bool *log_arguments)
{
    std::lock_guard<std::mutex> lock(g_state_mutex);
    *max_response_bytes = globals.config.max_response_bytes;
    *log_arguments = globals.config.log_arguments;

    if (hard_denied_commands().count(command) != 0) {
        return false;
    }
    return configured_command_allowed(globals.config, command);
}

static bool parse_unsigned_value(const char *text,
                                 const char *name,
                                 std::uint64_t minimum,
                                 std::uint64_t maximum,
                                 std::uint64_t *output)
{
    if (!text) {
        return false;
    }

    const std::string trimmed = str_trim(text);
    if (trimmed.empty() || trimmed[0] == '-') {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
                          "gRPC API: invalid value for '%s': '%s'\n",
                          name, text);
        return false;
    }

    errno = 0;
    char *end = nullptr;
    const unsigned long long value =
        std::strtoull(trimmed.c_str(), &end, 10);
    if (errno == ERANGE || end == trimmed.c_str() || !end || *end != '\0' ||
        value < minimum || value > maximum) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
                          "gRPC API: invalid value for '%s': '%s' "
                          "(expected %llu..%llu)\n",
                          name, text,
                          static_cast<unsigned long long>(minimum),
                          static_cast<unsigned long long>(maximum));
        return false;
    }

    *output = static_cast<std::uint64_t>(value);
    return true;
}

static bool parse_boolean_value(const char *text,
                                const char *name,
                                bool *output)
{
    const std::string value = to_lower_ascii(str_trim(text ? text : ""));
    if (value == "true" || value == "yes" || value == "on" || value == "1") {
        *output = true;
        return true;
    }
    if (value == "false" || value == "no" || value == "off" || value == "0") {
        *output = false;
        return true;
    }

    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
                      "gRPC API: invalid boolean for '%s': '%s'\n",
                      name, text ? text : "");
    return false;
}

static bool parse_allowed_commands(const char *text, ModuleConfig *config)
{
    std::string value = text ? text : "";
    for (char &c : value) {
        if (c == ',' || c == ';' || std::isspace(static_cast<unsigned char>(c))) {
            c = ' ';
        }
    }

    std::istringstream stream(value);
    std::string token;
    std::unordered_set<std::string> commands;
    while (stream >> token) {
        token = to_lower_ascii(str_trim(token));
        if (token == "*") {
            switch_log_printf(
                SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
                "gRPC API: wildcard '*' is not allowed in allowed-commands; "
                "use an explicit command list\n");
            return false;
        }
        if (token.find('*') != std::string::npos && token != "uuid_*") {
            switch_log_printf(
                SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
                "gRPC API: unsupported wildcard rule '%s'; only the "
                "controlled uuid_* family rule is supported\n",
                token.c_str());
            return false;
        }
        if (token == "uuid_*") {
            commands.insert(token);
            continue;
        }
        if (!valid_command_name(token)) {
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
                              "gRPC API: invalid command in allowed-commands: '%s'\n",
                              token.c_str());
            return false;
        }
        commands.insert(token);
    }

    if (commands.empty()) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
                          "gRPC API: allowed-commands must contain at least one "
                          "command\n");
        return false;
    }

    config->allowed_commands = std::move(commands);
    return true;
}

static bool read_config_file(ModuleConfig *output)
{
    ModuleConfig config = make_default_config();
    switch_xml_t cfg = nullptr;
    switch_xml_t xml = switch_xml_open_cfg("grpc_api.conf", &cfg, nullptr);
    if (!xml) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
                          "gRPC API: could not open grpc_api.conf\n");
        return false;
    }

    bool valid = true;
    switch_xml_t settings = switch_xml_child(cfg, "settings");
    for (switch_xml_t param = settings ? switch_xml_child(settings, "param") : nullptr;
         param; param = param->next) {
        const char *name = switch_xml_attr_soft(param, "name");
        const char *value = switch_xml_attr_soft(param, "value");
        std::uint64_t parsed = 0;

        if (!std::strcmp(name, "listen-address")) {
            const std::string address = str_trim(value);
            if (address.empty() || address.size() > 255) {
                switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
                                  "gRPC API: invalid listen-address\n");
                valid = false;
            } else {
                config.listen_address = address;
            }
        } else if (!std::strcmp(name, "worker-threads")) {
            const bool parsed_ok =
                parse_unsigned_value(value, name, 1, 128, &parsed);
            valid = parsed_ok && valid;
            if (parsed_ok) {
                config.worker_threads = static_cast<std::size_t>(parsed);
            }
        } else if (!std::strcmp(name, "max-concurrent-requests")) {
            const bool parsed_ok =
                parse_unsigned_value(value, name, 0, 10000, &parsed);
            valid = parsed_ok && valid;
            if (parsed_ok) {
                config.max_concurrent_requests =
                    static_cast<std::uint32_t>(parsed);
            }
        } else if (!std::strcmp(name, "queue-capacity")) {
            const bool parsed_ok =
                parse_unsigned_value(value, name, 1, 4096, &parsed);
            valid = parsed_ok && valid;
            if (parsed_ok) {
                config.queue_capacity = static_cast<std::size_t>(parsed);
            }
        } else if (!std::strcmp(name, "acceptor-count")) {
            const bool parsed_ok =
                parse_unsigned_value(value, name, 1, 64, &parsed);
            valid = parsed_ok && valid;
            if (parsed_ok) {
                config.acceptor_count = static_cast<std::size_t>(parsed);
            }
        } else if (!std::strcmp(name, "max-response-bytes")) {
            const bool parsed_ok = parse_unsigned_value(
                value, name, kMinResponseBytes, kMaxResponseBytes, &parsed);
            valid = parsed_ok && valid;
            if (parsed_ok) {
                config.max_response_bytes = static_cast<std::size_t>(parsed);
            }
        } else if (!std::strcmp(name, "shutdown-grace-ms")) {
            const bool parsed_ok =
                parse_unsigned_value(value, name, 0, 60000, &parsed);
            valid = parsed_ok && valid;
            if (parsed_ok) {
                config.shutdown_grace_ms = static_cast<std::uint32_t>(parsed);
            }
        } else if (!std::strcmp(name, "enable-reflection")) {
            bool enabled = false;
            const bool parsed_ok = parse_boolean_value(value, name, &enabled);
            valid = parsed_ok && valid;
            if (parsed_ok) {
                config.enable_reflection = enabled;
            }
        } else if (!std::strcmp(name, "log-arguments")) {
            bool enabled = false;
            const bool parsed_ok = parse_boolean_value(value, name, &enabled);
            valid = parsed_ok && valid;
            if (parsed_ok) {
                config.log_arguments = enabled;
            }
        } else if (!std::strcmp(name, "allowed-commands")) {
            const bool parsed_ok = parse_allowed_commands(value, &config);
            valid = parsed_ok && valid;
        } else {
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
                              "gRPC API: unknown configuration parameter '%s'\n",
                              name);
        }
    }

    switch_xml_free(xml);
    if (!valid) {
        return false;
    }

    *output = std::move(config);
    return true;
}

static std::string allowed_commands_text(const ModuleConfig &config)
{
    std::vector<std::string> commands(config.allowed_commands.begin(),
                                      config.allowed_commands.end());
    std::sort(commands.begin(), commands.end());
    std::ostringstream output;
    for (std::size_t i = 0; i < commands.size(); ++i) {
        if (i) {
            output << ',';
        }
        output << commands[i];
    }
    return output.str();
}

static bool appears_loopback_or_unix(const std::string &address)
{
    const std::string lowered = to_lower_ascii(address);
    return lowered.rfind("127.", 0) == 0 ||
           lowered.rfind("localhost:", 0) == 0 ||
           lowered.rfind("[::1]:", 0) == 0 ||
           lowered.rfind("unix:", 0) == 0;
}

struct CallData {
    enum class State { READY, PROCESSING, FINISH };

    FreeSwitchApi::AsyncService *service;
    ServerCompletionQueue *cq;
    ServerContext context;
    ApiRequest request;
    ApiResponse response;
    grpc::ServerAsyncResponseWriter<ApiResponse> responder;
    std::atomic<State> state{State::READY};
    std::atomic<bool> finish_started{false};
    bool counted = false;

    CallData(FreeSwitchApi::AsyncService *service_in,
             ServerCompletionQueue *cq_in)
        : service(service_in), cq(cq_in), responder(&context)
    {
        service->RequestExecute(&context, &request, &responder,
                                cq, cq, this);
    }

    void finish(const Status &status)
    {
        bool expected = false;
        if (!finish_started.compare_exchange_strong(expected, true)) {
            return;
        }
        state.store(State::FINISH, std::memory_order_release);
        responder.Finish(response, status, this);
    }
};

static bool create_acceptor(FreeSwitchApi::AsyncService *service,
                            ServerCompletionQueue *cq)
{
    try {
        new CallData(service, cq);
        return true;
    } catch (const std::exception &error) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
                          "gRPC API: unable to create RPC acceptor: %s\n",
                          error.what());
    } catch (...) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
                          "gRPC API: unable to create RPC acceptor\n");
    }
    return false;
}

enum class GateResult { ACCEPTED, BUSY, SHUTTING_DOWN };

static GateResult acquire_request_slot(CallData *call)
{
    std::lock_guard<std::mutex> lock(g_state_mutex);
    if (g_shutting_down.load(std::memory_order_acquire)) {
        return GateResult::SHUTTING_DOWN;
    }
    if (globals.config.max_concurrent_requests > 0 &&
        g_active_requests >= globals.config.max_concurrent_requests) {
        return GateResult::BUSY;
    }
    ++g_active_requests;
    call->counted = true;
    return GateResult::ACCEPTED;
}

static void release_request_slot(CallData *call)
{
    std::lock_guard<std::mutex> lock(g_state_mutex);
    if (!call->counted) {
        return;
    }
    if (g_active_requests > 0) {
        --g_active_requests;
    }
    call->counted = false;
}

static void finish_error(CallData *call,
                         StatusCode code,
                         const std::string &message,
                         const std::string &status_message)
{
    call->response.Clear();
    const std::string output = "-ERR " + message + "\n";
    call->response.set_success(false);
    call->response.set_message(output);
    call->response.set_format("plain");
    fill_plain(output, false, call->response.mutable_plain());
    call->finish(Status(code, status_message));
}

enum class TaskAction { RUN, CANCEL, INTERNAL_FAILURE };

class ThreadPool {
public:
    using Task = std::function<void(TaskAction)>;

    ThreadPool(std::size_t worker_count, std::size_t capacity)
        : capacity_(capacity)
    {
        workers_.reserve(worker_count);
        try {
            for (std::size_t i = 0; i < worker_count; ++i) {
                workers_.emplace_back(&ThreadPool::worker_loop, this);
            }
        } catch (...) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                accepting_ = false;
                stopping_ = true;
            }
            condition_.notify_all();
            for (std::thread &worker : workers_) {
                if (worker.joinable()) {
                    worker.join();
                }
            }
            throw;
        }
    }

    ~ThreadPool()
    {
        stop_and_join();
    }

    ThreadPool(const ThreadPool &) = delete;
    ThreadPool &operator=(const ThreadPool &) = delete;

    bool post(Task task)
    {
        try {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (!accepting_ || queue_.size() >= capacity_) {
                    return false;
                }
                queue_.emplace_back(std::move(task));
            }
            condition_.notify_one();
            return true;
        } catch (const std::exception &error) {
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
                              "gRPC API: unable to enqueue task: %s\n",
                              error.what());
            return false;
        } catch (...) {
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
                              "gRPC API: unable to enqueue task\n");
            return false;
        }
    }

    void begin_shutdown()
    {
        std::deque<Task> pending;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stopping_) {
                return;
            }
            accepting_ = false;
            stopping_ = true;
            pending.swap(queue_);
        }

        for (Task &task : pending) {
            try {
                task(TaskAction::CANCEL);
            } catch (const std::exception &error) {
                switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
                                  "gRPC API: queued-task cancellation failed: %s\n",
                                  error.what());
            } catch (...) {
                switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
                                  "gRPC API: queued-task cancellation failed\n");
            }
        }
        condition_.notify_all();
    }

    void join()
    {
        std::call_once(join_once_, [this]() {
            for (std::thread &worker : workers_) {
                if (worker.joinable()) {
                    worker.join();
                }
            }
        });
    }

    void stop_and_join()
    {
        begin_shutdown();
        join();
    }

    std::size_t queued() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.size();
    }

    std::size_t capacity() const
    {
        return capacity_;
    }

    std::size_t worker_count() const
    {
        return workers_.size();
    }

private:
    void worker_loop()
    {
        for (;;) {
            Task task;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                condition_.wait(lock, [this]() {
                    return stopping_ || !queue_.empty();
                });
                if (stopping_ && queue_.empty()) {
                    return;
                }
                task = std::move(queue_.front());
                queue_.pop_front();
            }

            try {
                task(TaskAction::RUN);
            } catch (const std::exception &error) {
                switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
                                  "gRPC API: worker exception: %s\n",
                                  error.what());
                try {
                    task(TaskAction::INTERNAL_FAILURE);
                } catch (...) {
                    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_CRIT,
                                      "gRPC API: failed to complete request after "
                                      "worker exception\n");
                }
            } catch (...) {
                switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
                                  "gRPC API: unknown worker exception\n");
                try {
                    task(TaskAction::INTERNAL_FAILURE);
                } catch (...) {
                    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_CRIT,
                                      "gRPC API: failed to complete request after "
                                      "worker exception\n");
                }
            }
        }
    }

    const std::size_t capacity_;
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::deque<Task> queue_;
    std::vector<std::thread> workers_;
    std::once_flag join_once_;
    bool accepting_ = true;
    bool stopping_ = false;
};

struct BackgroundApiCommand {
    std::string command;
    std::string arguments;
};

static bool parse_background_api_command(const std::string &input,
                                         BackgroundApiCommand *output,
                                         std::string *error)
{
    const std::string trimmed = str_trim(input);
    if (trimmed.empty()) {
        *error = "bgapi requires: <command> [arguments]";
        return false;
    }

    const std::size_t separator = trimmed.find_first_of(" \t\r\n");
    output->command = to_lower_ascii(
        separator == std::string::npos
            ? trimmed
            : trimmed.substr(0, separator));
    output->arguments = separator == std::string::npos
        ? ""
        : str_trim(trimmed.substr(separator + 1));

    if (!valid_command_name(output->command)) {
        *error = "bgapi contains an invalid nested command name";
        return false;
    }
    if (output->command == "bgapi") {
        *error = "nested bgapi is not supported";
        return false;
    }
    return true;
}

static std::string create_job_uuid()
{
    switch_uuid_t uuid;
    char uuid_text[SWITCH_UUID_FORMATTED_LENGTH + 1] = {};
    switch_uuid_get(&uuid);
    switch_uuid_format(uuid_text, &uuid);
    return uuid_text;
}

static void fire_background_job_event(const std::string &job_uuid,
                                      const std::string &command,
                                      const std::string &arguments,
                                      const std::string &result,
                                      bool command_ok,
                                      bool truncated)
{
    switch_event_t *event = nullptr;
    if (switch_event_create(&event, SWITCH_EVENT_BACKGROUND_JOB) !=
        SWITCH_STATUS_SUCCESS) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
                          "gRPC API: unable to create BACKGROUND_JOB event "
                          "for %s\n",
                          job_uuid.c_str());
        return;
    }

    switch_event_add_header_string(event, SWITCH_STACK_BOTTOM,
                                   "Job-UUID", job_uuid.c_str());
    switch_event_add_header_string(event, SWITCH_STACK_BOTTOM,
                                   "Job-Command", command.c_str());
    if (!arguments.empty()) {
        switch_event_add_header_string(event, SWITCH_STACK_BOTTOM,
                                       "Job-Command-Arg", arguments.c_str());
    }
    switch_event_add_header_string(event, SWITCH_STACK_BOTTOM,
                                   "Job-Success",
                                   command_ok ? "true" : "false");
    switch_event_add_header_string(event, SWITCH_STACK_BOTTOM,
                                   "Job-Truncated",
                                   truncated ? "true" : "false");
    switch_event_add_body(event, "%s", result.c_str());
    switch_event_fire(&event);
}

static void finish_plain_response(CallData *call,
                                  bool success,
                                  const std::string &output)
{
    call->response.Clear();
    call->response.set_success(success);
    call->response.set_message(output);
    call->response.set_format("plain");
    fill_plain(output, success, call->response.mutable_plain());
    call->finish(Status::OK);
}

class BackgroundJobCounterGuard {
public:
    ~BackgroundJobCounterGuard()
    {
        g_background_jobs.fetch_sub(1, std::memory_order_acq_rel);
    }
};

static void execute_background_job(const std::string &job_uuid,
                                   const std::string &command,
                                   const std::string &arguments,
                                   std::size_t max_response_bytes,
                                   bool log_arguments,
                                   TaskAction action) noexcept
{
    BackgroundJobCounterGuard counter_guard;

    try {
        std::string output;
        bool command_ok = false;

        if (action == TaskAction::CANCEL) {
            output = "-ERR Background job canceled: module shutting down\n";
        } else if (action == TaskAction::INTERNAL_FAILURE) {
            output = "-ERR Background job failed in worker\n";
        } else {
            if (log_arguments) {
                switch_log_printf(
                    SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG,
                    "gRPC API: bgapi job=%s command=%s args=%s\n",
                    job_uuid.c_str(), command.c_str(),
                    sanitize_log_value(arguments).c_str());
            } else {
                switch_log_printf(
                    SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG,
                    "gRPC API: bgapi job=%s command=%s\n",
                    job_uuid.c_str(), command.c_str());
            }

            ApiExecutionResult result = execute_api_command(command, arguments);
            command_ok = api_result_succeeded(result);
            output = std::move(result.raw);
            if (output.empty()) {
                output = command_ok
                    ? "Command returned no output!\n"
                    : "-ERR FreeSWITCH API execution failed\n";
            }
        }

        const bool truncated =
            truncate_response(&output, max_response_bytes);
        fire_background_job_event(job_uuid, command, arguments, output,
                                  command_ok, truncated);
    } catch (const std::exception &error) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
                          "gRPC API: background job %s failed: %s\n",
                          job_uuid.c_str(), error.what());
        try {
            fire_background_job_event(
                job_uuid, command, arguments,
                "-ERR Background job failed with an internal exception\n",
                false, false);
        } catch (...) {
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_CRIT,
                              "gRPC API: unable to publish failure for "
                              "background job %s\n",
                              job_uuid.c_str());
        }
    } catch (...) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
                          "gRPC API: background job %s failed\n",
                          job_uuid.c_str());
        try {
            fire_background_job_event(
                job_uuid, command, arguments,
                "-ERR Background job failed with an internal exception\n",
                false, false);
        } catch (...) {
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_CRIT,
                              "gRPC API: unable to publish failure for "
                              "background job %s\n",
                              job_uuid.c_str());
        }
    }
}

static void execute_bgapi_request(CallData *call,
                                  const std::string &arguments,
                                  ThreadPool *thread_pool)
{
    BackgroundApiCommand nested;
    std::string error;
    if (!parse_background_api_command(arguments, &nested, &error)) {
        finish_error(call, StatusCode::INVALID_ARGUMENT,
                     error, "invalid bgapi arguments");
        return;
    }

    std::size_t max_response_bytes = 0;
    bool log_arguments = false;
    if (!command_allowed(nested.command, &max_response_bytes,
                         &log_arguments)) {
        finish_error(call, StatusCode::PERMISSION_DENIED,
                     "Nested bgapi command is not permitted by grpc_api.conf",
                     "nested command not permitted");
        return;
    }

    const std::string job_uuid = create_job_uuid();
    g_background_jobs.fetch_add(1, std::memory_order_acq_rel);

    const bool posted = thread_pool && thread_pool->post(
        [job_uuid,
         command = std::move(nested.command),
         arguments = std::move(nested.arguments),
         max_response_bytes,
         log_arguments](TaskAction action) {
            execute_background_job(job_uuid, command, arguments,
                                   max_response_bytes, log_arguments, action);
        });

    if (!posted) {
        g_background_jobs.fetch_sub(1, std::memory_order_acq_rel);
        if (g_shutting_down.load(std::memory_order_acquire)) {
            finish_error(call, StatusCode::UNAVAILABLE,
                         "Server shutting down", "server shutting down");
        } else {
            finish_error(call, StatusCode::RESOURCE_EXHAUSTED,
                         "Worker queue full", "worker queue full");
        }
        return;
    }

    finish_plain_response(call, true,
                          "+OK Job-UUID: " + job_uuid + "\n");
}

static void execute_request(CallData *call,
                            const std::string &command,
                            const std::string &arguments,
                            std::size_t max_response_bytes,
                            bool log_arguments,
                            ThreadPool *thread_pool)
{
    if (command == "bgapi") {
        execute_bgapi_request(call, arguments, thread_pool);
        return;
    }

    if (log_arguments) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG,
                          "gRPC API: command=%s args=%s\n",
                          command.c_str(), sanitize_log_value(arguments).c_str());
    } else {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG,
                          "gRPC API: command=%s\n", command.c_str());
    }

    ApiExecutionResult result = execute_api_command(command, arguments);

    bool command_ok = api_result_succeeded(result);
    if (result.raw.empty() && !command_ok) {
        result.raw = "-ERR FreeSWITCH API execution failed\n";
    }

    const bool truncated =
        truncate_response(&result.raw, max_response_bytes);

    call->response.Clear();
    call->response.set_success(command_ok);
    call->response.set_truncated(truncated);

    const fsgrpc::ResponseMode mode = call->request.response_mode();
    const bool include_raw = mode != fsgrpc::RESPONSE_MODE_STRUCTURED_ONLY;
    const bool include_structured = mode != fsgrpc::RESPONSE_MODE_RAW_ONLY;

    if (include_raw) {
        call->response.set_message(result.raw);
    }

    if (include_structured) {
        if (truncated || result.raw.size() > kMaxStructuredParseBytes) {
            // Parsing very large tables/JSON can expand memory substantially.
            // Keep the oneof useful without duplicating a huge parsed graph.
            call->response.set_format("plain");
            fill_plain(result.raw, command_ok, call->response.mutable_plain());
        } else {
            route_response(result.content_type, result.raw,
                           command_ok, &call->response);
        }

        if (call->response.ByteSizeLong() >
            max_wire_response_bytes(max_response_bytes)) {
            call->response.clear_data();
            call->response.set_format("plain");
            fill_plain(result.raw, command_ok, call->response.mutable_plain());
        }
    } else {
        call->response.set_format(detect_format(result.content_type, result.raw));
    }

    call->finish(Status::OK);
}

static bool reload_dynamic_config()
{
    ModuleConfig new_config;
    if (!read_config_file(&new_config)) {
        return false;
    }

    bool restart_required = false;
    {
        std::lock_guard<std::mutex> lock(g_state_mutex);
        restart_required =
            new_config.listen_address != globals.config.listen_address ||
            new_config.worker_threads != globals.config.worker_threads ||
            new_config.queue_capacity != globals.config.queue_capacity ||
            new_config.acceptor_count != globals.config.acceptor_count ||
            new_config.max_response_bytes != globals.config.max_response_bytes ||
            new_config.enable_reflection != globals.config.enable_reflection;

        globals.config.max_concurrent_requests =
            new_config.max_concurrent_requests;
        globals.config.shutdown_grace_ms = new_config.shutdown_grace_ms;
        globals.config.log_arguments = new_config.log_arguments;
        globals.config.allowed_commands =
            std::move(new_config.allowed_commands);
    }

    if (restart_required) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE,
                          "gRPC API: startup-only configuration changed; "
                          "unload and load mod_grpc_api to apply it\n");
    }

    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE,
                      "gRPC API: dynamic configuration reloaded\n");
    return true;
}

} // namespace

SWITCH_STANDARD_API(grpc_api_command)
{
    (void)session;

    if (zstr(cmd) || !strcasecmp(cmd, "help")) {
        stream->write_function(
            stream,
            "+OK Usage: grpc_api <status|reload>\n"
            "  status - show runtime configuration and queue state\n"
            "  reload - reload max-concurrent-requests, shutdown-grace-ms,\n"
            "           allowed-commands and log-arguments\n");
        return SWITCH_STATUS_SUCCESS;
    }

    if (!strcasecmp(cmd, "reload")) {
        if (g_shutting_down.load(std::memory_order_acquire)) {
            stream->write_function(stream, "-ERR Module is shutting down\n");
        } else if (reload_dynamic_config()) {
            stream->write_function(stream, "+OK Configuration reloaded\n");
        } else {
            stream->write_function(stream,
                                   "-ERR Configuration reload failed; "
                                   "previous values retained\n");
        }
        return SWITCH_STATUS_SUCCESS;
    }

    if (!strcasecmp(cmd, "status")) {
        ModuleConfig config;
        std::uint32_t active = 0;
        {
            std::lock_guard<std::mutex> lock(g_state_mutex);
            config = globals.config;
            active = g_active_requests;
        }

        std::shared_ptr<ThreadPool> pool;
        {
            std::lock_guard<std::mutex> lock(g_lifecycle_mutex);
            pool = globals.thread_pool;
        }

        stream->write_function(
            stream,
            "+OK listen=%s workers=%llu queue=%llu/%llu acceptors=%llu "
            "active=%u background_jobs=%u max_concurrent=%u "
            "max_response_bytes=%llu "
            "reflection=%s log_arguments=%s shutting_down=%s "
            "allowed_commands=%s\n",
            config.listen_address.c_str(),
            static_cast<unsigned long long>(pool ? pool->worker_count()
                                                  : config.worker_threads),
            static_cast<unsigned long long>(pool ? pool->queued() : 0),
            static_cast<unsigned long long>(pool ? pool->capacity()
                                                  : config.queue_capacity),
            static_cast<unsigned long long>(config.acceptor_count),
            active,
            g_background_jobs.load(std::memory_order_acquire),
            config.max_concurrent_requests,
            static_cast<unsigned long long>(config.max_response_bytes),
            config.enable_reflection ? "true" : "false",
            config.log_arguments ? "true" : "false",
            g_shutting_down.load(std::memory_order_acquire) ? "true" : "false",
            allowed_commands_text(config).c_str());
        return SWITCH_STATUS_SUCCESS;
    }

    stream->write_function(stream, "-ERR Unknown command: %s\n", cmd);
    return SWITCH_STATUS_SUCCESS;
}

SWITCH_MODULE_LOAD_FUNCTION(mod_grpc_api_load);
SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_grpc_api_shutdown);
SWITCH_MODULE_RUNTIME_FUNCTION(mod_grpc_api_runtime);
SWITCH_MODULE_DEFINITION(mod_grpc_api, mod_grpc_api_load,
                         mod_grpc_api_shutdown, mod_grpc_api_runtime);

SWITCH_MODULE_LOAD_FUNCTION(mod_grpc_api_load)
{
    ModuleConfig startup_config;
    if (!read_config_file(&startup_config)) {
        switch_log_printf(
            SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
            "gRPC API: module load failed because grpc_api.conf is invalid\n");
        return SWITCH_STATUS_FALSE;
    }

    {
        std::lock_guard<std::mutex> state_lock(g_state_mutex);
        globals.config = std::move(startup_config);
        g_active_requests = 0;
    }
    g_background_jobs.store(0, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lifecycle_lock(g_lifecycle_mutex);
        globals.server.reset();
        globals.cq.reset();
        globals.thread_pool.reset();
    }
    g_shutting_down.store(false, std::memory_order_release);
    g_shutdown_sequence_started.store(false, std::memory_order_release);

    *module_interface =
        switch_loadable_module_create_module_interface(pool, modname);

    switch_api_interface_t *api_interface = nullptr;
    SWITCH_ADD_API(api_interface, "grpc_api",
                   "Control the asynchronous gRPC API module",
                   grpc_api_command, "[status|reload]");

    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE,
                      "mod_grpc_api loaded\n");
    return SWITCH_STATUS_SUCCESS;
}

SWITCH_MODULE_RUNTIME_FUNCTION(mod_grpc_api_runtime)
{
    ModuleConfig config;
    {
        std::lock_guard<std::mutex> lock(g_state_mutex);
        config = globals.config;
    }

#ifndef MOD_GRPC_API_HAVE_REFLECTION
    if (config.enable_reflection) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
                          "gRPC API: reflection was enabled, but the build does not "
                          "include grpc++_reflection\n");
        return SWITCH_STATUS_TERM;
    }
#endif

    {
        std::lock_guard<std::mutex> lock(g_state_mutex);
        globals.config = config;
    }

    if (!appears_loopback_or_unix(config.listen_address)) {
        switch_log_printf(
            SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
            "gRPC API: %s is not a loopback/unix address. The module uses "
            "insecure gRPC transport; protect remote access with a firewall "
            "or an authenticated TLS proxy.\n",
            config.listen_address.c_str());
    }

#ifdef MOD_GRPC_API_HAVE_REFLECTION
    if (config.enable_reflection) {
        grpc::reflection::InitProtoReflectionServerBuilderPlugin();
    }
#endif

    FreeSwitchApi::AsyncService service;
    ServerBuilder builder;

    int selected_port = 0;
    builder.AddListeningPort(config.listen_address,
                             grpc::InsecureServerCredentials(),
                             &selected_port);
    builder.RegisterService(&service);
    builder.SetMaxReceiveMessageSize(kMaxReceiveMessageBytes);
    builder.SetMaxSendMessageSize(static_cast<int>(
        max_wire_response_bytes(config.max_response_bytes)));

    std::unique_ptr<ServerCompletionQueue> cq_unique =
        builder.AddCompletionQueue();
    std::unique_ptr<Server> server_unique = builder.BuildAndStart();
    if (!cq_unique || !server_unique) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
                          "gRPC API: failed to bind/start server on %s\n",
                          config.listen_address.c_str());
        if (server_unique) {
            server_unique->Shutdown();
        }
        if (cq_unique) {
            cq_unique->Shutdown();
        }
        return SWITCH_STATUS_TERM;
    }

    std::shared_ptr<ServerCompletionQueue> cq(std::move(cq_unique));
    std::shared_ptr<Server> server(std::move(server_unique));
    std::shared_ptr<ThreadPool> thread_pool;

    try {
        thread_pool = std::make_shared<ThreadPool>(config.worker_threads,
                                                   config.queue_capacity);
    } catch (const std::exception &error) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
                          "gRPC API: failed to create worker pool: %s\n",
                          error.what());
        server->Shutdown();
        cq->Shutdown();
        server.reset();
        cq.reset();
        return SWITCH_STATUS_TERM;
    }

    std::size_t acceptors_created = 0;
    {
        std::lock_guard<std::mutex> lock(g_lifecycle_mutex);
        if (g_shutting_down.load(std::memory_order_acquire)) {
            thread_pool->stop_and_join();
            server->Shutdown();
            cq->Shutdown();
            server.reset();
            cq.reset();
            return SWITCH_STATUS_TERM;
        }

        globals.server = server;
        globals.cq = cq;
        globals.thread_pool = thread_pool;

        for (std::size_t i = 0; i < config.acceptor_count; ++i) {
            if (create_acceptor(&service, cq.get())) {
                ++acceptors_created;
            }
        }
    }

    if (acceptors_created == 0) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
                          "gRPC API: no RPC acceptors were created\n");
        thread_pool->begin_shutdown();
        server->Shutdown();
        thread_pool->join();
        cq->Shutdown();
        {
            std::lock_guard<std::mutex> lock(g_lifecycle_mutex);
            globals.thread_pool.reset();
            globals.server.reset();
            globals.cq.reset();
        }
        return SWITCH_STATUS_TERM;
    }

    switch_log_printf(
        SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE,
        "gRPC API: async server listening on %s (port=%d, workers=%llu, "
        "queue=%llu, max_concurrent=%u, acceptors=%llu)\n",
        config.listen_address.c_str(), selected_port,
        static_cast<unsigned long long>(config.worker_threads),
        static_cast<unsigned long long>(config.queue_capacity),
        config.max_concurrent_requests,
        static_cast<unsigned long long>(acceptors_created));

    void *tag = nullptr;
    bool ok = false;
    while (cq->Next(&tag, &ok)) {
        CallData *call = static_cast<CallData *>(tag);
        const CallData::State state =
            call->state.load(std::memory_order_acquire);

        if (state == CallData::State::FINISH) {
            release_request_slot(call);
            delete call;
            continue;
        }

        if (state != CallData::State::READY) {
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
                              "gRPC API: unexpected completion-queue state\n");
            continue;
        }

        if (!ok) {
            delete call;
            continue;
        }

        {
            std::lock_guard<std::mutex> lock(g_lifecycle_mutex);
            if (!g_shutting_down.load(std::memory_order_acquire)) {
                create_acceptor(&service, cq.get());
            }
        }
        call->state.store(CallData::State::PROCESSING,
                          std::memory_order_release);

        const GateResult gate = acquire_request_slot(call);
        if (gate == GateResult::SHUTTING_DOWN) {
            finish_error(call, StatusCode::UNAVAILABLE,
                         "Server shutting down", "server shutting down");
            continue;
        }
        if (gate == GateResult::BUSY) {
            finish_error(call, StatusCode::RESOURCE_EXHAUSTED,
                         "Server busy", "concurrency limit reached");
            continue;
        }

        std::string command =
            to_lower_ascii(str_trim(call->request.command()));
        const std::string &arguments = call->request.arguments();

        if (!valid_command_name(command)) {
            finish_error(call, StatusCode::INVALID_ARGUMENT,
                         "Invalid command name", "invalid command name");
            continue;
        }
        if (arguments.size() > kMaxArgumentBytes ||
            arguments.find('\0') != std::string::npos) {
            finish_error(call, StatusCode::INVALID_ARGUMENT,
                         "Arguments are too large or contain a NUL byte",
                         "invalid arguments");
            continue;
        }

        std::size_t max_response_bytes = 0;
        bool log_arguments = false;
        if (!command_allowed(command, &max_response_bytes, &log_arguments)) {
            finish_error(call, StatusCode::PERMISSION_DENIED,
                         "Command not permitted by grpc_api.conf",
                         "command not permitted");
            continue;
        }

        const bool posted = thread_pool->post(
            [call,
             command = std::move(command),
             max_response_bytes,
             log_arguments,
             thread_pool_ptr = thread_pool.get()](TaskAction action) {
                if (action == TaskAction::CANCEL) {
                    finish_error(call, StatusCode::UNAVAILABLE,
                                 "Server shutting down",
                                 "server shutting down");
                    return;
                }
                if (action == TaskAction::INTERNAL_FAILURE) {
                    finish_error(call, StatusCode::INTERNAL,
                                 "Internal worker error",
                                 "internal worker error");
                    return;
                }
                execute_request(call, command, call->request.arguments(),
                                max_response_bytes, log_arguments,
                                thread_pool_ptr);
            });

        if (!posted) {
            if (g_shutting_down.load(std::memory_order_acquire)) {
                finish_error(call, StatusCode::UNAVAILABLE,
                             "Server shutting down", "server shutting down");
            } else {
                finish_error(call, StatusCode::RESOURCE_EXHAUSTED,
                             "Worker queue full", "worker queue full");
            }
        }
    }

    thread_pool->stop_and_join();

    {
        std::lock_guard<std::mutex> lock(g_lifecycle_mutex);
        if (globals.thread_pool == thread_pool) {
            globals.thread_pool.reset();
        }
        if (globals.server == server) {
            globals.server.reset();
        }
        if (globals.cq == cq) {
            globals.cq.reset();
        }
    }

    thread_pool.reset();
    server.reset();
    cq.reset();

    std::uint32_t remaining = 0;
    {
        std::lock_guard<std::mutex> lock(g_state_mutex);
        remaining = g_active_requests;
    }
    if (remaining != 0) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
                          "gRPC API: runtime exited with %u active request(s)\n",
                          remaining);
    }

    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE,
                      "gRPC API: async runtime stopped\n");
    return SWITCH_STATUS_TERM;
}

SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_grpc_api_shutdown)
{
    g_shutting_down.store(true, std::memory_order_release);

    bool expected = false;
    if (!g_shutdown_sequence_started.compare_exchange_strong(expected, true)) {
        return SWITCH_STATUS_SUCCESS;
    }

    std::uint32_t grace_ms = 5000;
    {
        std::lock_guard<std::mutex> lock(g_state_mutex);
        grace_ms = globals.config.shutdown_grace_ms;
    }

    std::shared_ptr<Server> server;
    std::shared_ptr<ServerCompletionQueue> cq;
    std::shared_ptr<ThreadPool> thread_pool;
    {
        std::lock_guard<std::mutex> lock(g_lifecycle_mutex);
        server = globals.server;
        cq = globals.cq;
        thread_pool = globals.thread_pool;
    }

    if (thread_pool) {
        thread_pool->begin_shutdown();
    }

    if (server) {
        server->Shutdown(std::chrono::system_clock::now() +
                         std::chrono::milliseconds(grace_ms));
    }

    if (thread_pool) {
        thread_pool->join();
    }

    if (cq) {
        cq->Shutdown();
    }

    thread_pool.reset();
    server.reset();
    cq.reset();

    return SWITCH_STATUS_SUCCESS;
}
