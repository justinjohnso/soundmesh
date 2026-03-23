#include <unity.h>

#include <ctype.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "control/json_extract.h"
#include "../../../lib/control/src/json_extract.c"

#define CONTRACT_MAX_NODES 32

typedef struct {
    int status;
    const char *error;
    bool ok;
} api_result_t;

static const char *skip_ws(const char *p)
{
    while (p && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) {
        p++;
    }
    return p;
}

static const char *find_matching_delim(const char *start, char open, char close)
{
    if (!start || *start != open) {
        return NULL;
    }

    int depth = 0;
    bool in_string = false;
    bool escape = false;
    for (const char *p = start; *p; p++) {
        if (in_string) {
            if (escape) {
                escape = false;
                continue;
            }
            if (*p == '\\') {
                escape = true;
                continue;
            }
            if (*p == '"') {
                in_string = false;
            }
            continue;
        }

        if (*p == '"') {
            in_string = true;
            continue;
        }
        if (*p == open) {
            depth++;
        } else if (*p == close) {
            depth--;
            if (depth == 0) {
                return p;
            }
        }
    }
    return NULL;
}

static bool parse_string_at(const char *value_start, char *out, size_t out_size, const char **next_out)
{
    if (!value_start || *value_start != '"' || !out || out_size == 0) {
        return false;
    }

    size_t idx = 0;
    bool escape = false;
    const char *p = value_start + 1;
    while (*p) {
        if (!escape && *p == '"') {
            out[idx] = '\0';
            if (next_out) {
                *next_out = p + 1;
            }
            return true;
        }
        if (!escape && *p == '\\') {
            escape = true;
            p++;
            continue;
        }
        if (idx + 1 >= out_size) {
            return false;
        }
        out[idx++] = *p;
        escape = false;
        p++;
    }
    return false;
}

static bool parse_bool_at(const char *value_start, bool *out)
{
    if (strncmp(value_start, "true", 4) == 0) {
        *out = true;
        return true;
    }
    if (strncmp(value_start, "false", 5) == 0) {
        *out = false;
        return true;
    }
    return false;
}

static bool parse_null_at(const char *value_start)
{
    return value_start && strncmp(value_start, "null", 4) == 0;
}

static bool parse_int_at(const char *value_start, long *out, const char **next_out)
{
    if (!value_start || !out) {
        return false;
    }
    char *end = NULL;
    long value = strtol(value_start, &end, 10);
    if (end == value_start) {
        return false;
    }
    if (*end == '.' || *end == 'e' || *end == 'E') {
        return false;
    }
    *out = value;
    if (next_out) {
        *next_out = end;
    }
    return true;
}

static bool parse_number_at(const char *value_start, double *out, const char **next_out)
{
    if (!value_start || !out) {
        return false;
    }
    char *end = NULL;
    double value = strtod(value_start, &end);
    if (end == value_start || !isfinite(value)) {
        return false;
    }
    *out = value;
    if (next_out) {
        *next_out = end;
    }
    return true;
}

static bool find_key_value_start(const char *json, const char *key, const char **value_start_out)
{
    if (!json || !key || !value_start_out) {
        return false;
    }

    char pattern[64];
    int n = snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    if (n <= 0 || (size_t)n >= sizeof(pattern)) {
        return false;
    }

    const char *key_pos = strstr(json, pattern);
    if (!key_pos) {
        return false;
    }
    const char *colon = strchr(key_pos + n, ':');
    if (!colon) {
        return false;
    }
    *value_start_out = skip_ws(colon + 1);
    return true;
}

static bool field_string(const char *json, const char *key, char *out, size_t out_size)
{
    const char *value_start = NULL;
    return find_key_value_start(json, key, &value_start) && parse_string_at(value_start, out, out_size, NULL);
}

static bool field_bool(const char *json, const char *key, bool *out)
{
    const char *value_start = NULL;
    return find_key_value_start(json, key, &value_start) && parse_bool_at(value_start, out);
}

static bool field_int(const char *json, const char *key, long *out)
{
    const char *value_start = NULL;
    return find_key_value_start(json, key, &value_start) && parse_int_at(value_start, out, NULL);
}

static bool field_null_or_int(const char *json, const char *key, bool *is_null, long *value)
{
    const char *value_start = NULL;
    if (!find_key_value_start(json, key, &value_start)) {
        return false;
    }
    if (parse_null_at(value_start)) {
        *is_null = true;
        return true;
    }
    *is_null = false;
    return parse_int_at(value_start, value, NULL);
}

static bool field_null_or_number(const char *json, const char *key, bool *is_null, double *value)
{
    const char *value_start = NULL;
    if (!find_key_value_start(json, key, &value_start)) {
        return false;
    }
    if (parse_null_at(value_start)) {
        *is_null = true;
        return true;
    }
    *is_null = false;
    return parse_number_at(value_start, value, NULL);
}

static bool field_array_span(const char *json, const char *key, const char **start_out, const char **end_out)
{
    const char *value_start = NULL;
    if (!find_key_value_start(json, key, &value_start) || *value_start != '[') {
        return false;
    }
    const char *end = find_matching_delim(value_start, '[', ']');
    if (!end) {
        return false;
    }
    *start_out = value_start;
    *end_out = end;
    return true;
}

static bool field_object_span(const char *json, const char *key, const char **start_out, const char **end_out)
{
    const char *value_start = NULL;
    if (!find_key_value_start(json, key, &value_start) || *value_start != '{') {
        return false;
    }
    const char *end = find_matching_delim(value_start, '{', '}');
    if (!end) {
        return false;
    }
    *start_out = value_start;
    *end_out = end;
    return true;
}

static bool is_mac_string(const char *value)
{
    if (!value || strlen(value) != 17) {
        return false;
    }
    for (int i = 0; i < 17; i++) {
        if ((i + 1) % 3 == 0) {
            if (value[i] != ':') {
                return false;
            }
        } else if (!isdigit((unsigned char)value[i]) && !(value[i] >= 'A' && value[i] <= 'F')) {
            return false;
        }
    }
    return true;
}

static bool validate_uplink_contract(const char *json)
{
    bool enabled = false;
    bool configured = false;
    bool root_applied = false;
    bool pending_apply = false;
    char ssid[64] = {0};
    char last_error[96] = {0};
    long updated_ms = -1;

    if (!field_bool(json, "enabled", &enabled) ||
        !field_bool(json, "configured", &configured) ||
        !field_bool(json, "rootApplied", &root_applied) ||
        !field_bool(json, "pendingApply", &pending_apply) ||
        !field_string(json, "ssid", ssid, sizeof(ssid)) ||
        !field_string(json, "lastError", last_error, sizeof(last_error)) ||
        !field_int(json, "updatedMs", &updated_ms)) {
        return false;
    }

    (void)enabled;
    if (strlen(ssid) > 32) {
        return false;
    }
    if (strlen(last_error) > 47) {
        return false;
    }
    return updated_ms >= 0;
}

static bool validate_ota_contract(const char *json)
{
    bool enabled = false;
    bool in_progress = false;
    bool last_ok = false;
    char phase[64] = {0};
    char last_url[256] = {0};
    long last_err = 0;

    if (!field_bool(json, "enabled", &enabled) ||
        !field_bool(json, "inProgress", &in_progress) ||
        !field_string(json, "phase", phase, sizeof(phase)) ||
        !field_bool(json, "lastOk", &last_ok) ||
        !field_int(json, "lastErr", &last_err) ||
        !field_string(json, "lastUrl", last_url, sizeof(last_url))) {
        return false;
    }

    (void)enabled;
    (void)in_progress;
    (void)last_ok;
    if (strcmp(phase, "idle") != 0 &&
        strcmp(phase, "queued") != 0 &&
        strcmp(phase, "downloading") != 0 &&
        strcmp(phase, "failed") != 0 &&
        strcmp(phase, "restarting") != 0) {
        return false;
    }
    return strlen(last_url) < 192;
}

static bool validate_fft_bins_field(const char *json)
{
    const char *start = NULL;
    const char *end = NULL;
    bool is_null = false;
    double val = 0;
    if (field_null_or_number(json, "fftBins", &is_null, &val) && !is_null) {
        return false;
    }
    if (field_null_or_number(json, "fftBins", &is_null, &val) && is_null) {
        return true;
    }
    if (!field_array_span(json, "fftBins", &start, &end)) {
        return false;
    }
    const char *p = start + 1;
    while (p < end) {
        p = skip_ws(p);
        if (p >= end) {
            break;
        }
        if (*p == ',') {
            p++;
            continue;
        }
        double n = 0;
        const char *next = NULL;
        if (!parse_number_at(p, &n, &next)) {
            return false;
        }
        p = next;
    }
    return true;
}

static bool validate_status_contract(const char *json)
{
    long ts = -1;
    long heap_kb = -1;
    long latency_ms = -1;
    char self[24] = {0};
    char net_if[64] = {0};
    char build_label[16] = {0};
    char mesh_state[32] = {0};
    bool core0_is_null = false;
    long core0_load = -1;
    bool bpm_is_null = false;
    double bpm_number = 0;
    const char *nodes_start = NULL;
    const char *nodes_end = NULL;

    if (!field_int(json, "ts", &ts) ||
        !field_string(json, "self", self, sizeof(self)) ||
        !field_int(json, "heapKb", &heap_kb) ||
        !field_null_or_int(json, "core0LoadPct", &core0_is_null, &core0_load) ||
        !field_int(json, "latencyMs", &latency_ms) ||
        !field_string(json, "netIf", net_if, sizeof(net_if)) ||
        !field_string(json, "buildLabel", build_label, sizeof(build_label)) ||
        !field_string(json, "meshState", mesh_state, sizeof(mesh_state)) ||
        !field_null_or_number(json, "bpm", &bpm_is_null, &bpm_number) ||
        !field_array_span(json, "nodes", &nodes_start, &nodes_end)) {
        return false;
    }

    if (!validate_fft_bins_field(json)) {
        return false;
    }

    if (ts < 0 || heap_kb < 0 || latency_ms < 0 || !is_mac_string(self) || strlen(net_if) == 0) {
        return false;
    }
    if (!core0_is_null && (core0_load < 0 || core0_load > 100)) {
        return false;
    }
    if (strcmp(build_label, "SRC") != 0 && strcmp(build_label, "OUT") != 0 && strcmp(build_label, "UNKNOWN") != 0) {
        return false;
    }
    if (strcmp(mesh_state, "Mesh OK") != 0 && strcmp(mesh_state, "Mesh Degraded") != 0) {
        return false;
    }
    if (!bpm_is_null && !isfinite(bpm_number)) {
        return false;
    }

    char seen_macs[CONTRACT_MAX_NODES][18] = {{0}};
    int seen_count = 0;
    int node_count = 0;
    const char *p = nodes_start + 1;
    while (p < nodes_end) {
        p = skip_ws(p);
        if (p >= nodes_end) {
            break;
        }
        if (*p == ',') {
            p++;
            continue;
        }
        if (*p != '{') {
            return false;
        }
        const char *node_end = find_matching_delim(p, '{', '}');
        if (!node_end || node_end > nodes_end) {
            return false;
        }

        size_t node_len = (size_t)(node_end - p + 1);
        char node_json[512];
        if (node_len >= sizeof(node_json)) {
            return false;
        }
        memcpy(node_json, p, node_len);
        node_json[node_len] = '\0';

        char mac[24] = {0};
        char role[8] = {0};
        bool root = false;
        long layer = -1;
        long rssi = 0;
        long children = -1;
        bool streaming = false;
        bool stale = false;
        long uptime = -1;
        bool parent_is_null = false;
        double parent_num = 0;
        char parent_mac[24] = {0};

        if (!field_string(node_json, "mac", mac, sizeof(mac)) ||
            !field_string(node_json, "role", role, sizeof(role)) ||
            !field_bool(node_json, "root", &root) ||
            !field_int(node_json, "layer", &layer) ||
            !field_int(node_json, "rssi", &rssi) ||
            !field_int(node_json, "children", &children) ||
            !field_bool(node_json, "streaming", &streaming) ||
            !field_bool(node_json, "stale", &stale) ||
            !field_int(node_json, "uptime", &uptime)) {
            return false;
        }

        const char *parent_start = NULL;
        if (!find_key_value_start(node_json, "parent", &parent_start)) {
            return false;
        }
        if (parse_null_at(parent_start)) {
            parent_is_null = true;
        } else if (parse_number_at(parent_start, &parent_num, NULL)) {
            return false;
        } else if (!parse_string_at(parent_start, parent_mac, sizeof(parent_mac), NULL)) {
            return false;
        }

        if (!is_mac_string(mac) ||
            (strcmp(role, "TX") != 0 && strcmp(role, "RX") != 0) ||
            layer < 0 ||
            children < 0 ||
            uptime < 0) {
            return false;
        }
        if (root && layer != 0) {
            return false;
        }
        if (!parent_is_null && !is_mac_string(parent_mac)) {
            return false;
        }

        for (int i = 0; i < seen_count; i++) {
            if (strcmp(seen_macs[i], mac) == 0) {
                return false;
            }
        }
        if (seen_count >= CONTRACT_MAX_NODES) {
            return false;
        }
        strcpy(seen_macs[seen_count++], mac);
        node_count++;
        p = node_end + 1;
    }

    if (node_count > CONTRACT_MAX_NODES) {
        return false;
    }

    const char *uplink_start = NULL;
    const char *uplink_end = NULL;
    if (field_object_span(json, "uplink", &uplink_start, &uplink_end)) {
        size_t len = (size_t)(uplink_end - uplink_start + 1);
        char uplink_json[512];
        if (len >= sizeof(uplink_json)) {
            return false;
        }
        memcpy(uplink_json, uplink_start, len);
        uplink_json[len] = '\0';
        if (!validate_uplink_contract(uplink_json)) {
            return false;
        }
    }

    const char *ota_start = NULL;
    const char *ota_end = NULL;
    if (field_object_span(json, "ota", &ota_start, &ota_end)) {
        char enabled_dummy[8] = {0};
        char mode[16] = {0};
        size_t len = (size_t)(ota_end - ota_start + 1);
        char ota_json[256];
        if (len >= sizeof(ota_json)) {
            return false;
        }
        memcpy(ota_json, ota_start, len);
        ota_json[len] = '\0';
        if (!field_string(ota_json, "mode", mode, sizeof(mode))) {
            return false;
        }
        if (!find_key_value_start(ota_json, "enabled", (const char **)&enabled_dummy)) {
            return false;
        }
    }

    return true;
}

static api_result_t simulate_uplink_post(const char *body, bool apply_ok)
{
    if (!body || body[0] == '\0') {
        return (api_result_t){.status = 400, .error = "empty body", .ok = false};
    }

    bool enabled = false;
    if (!json_extract_bool_field(body, "enabled", &enabled)) {
        return (api_result_t){.status = 400, .error = "missing enabled", .ok = false};
    }

    char ssid[33] = {0};
    char password[65] = {0};
    if (enabled) {
        if (!json_extract_string_field(body, "ssid", ssid, sizeof(ssid)) || ssid[0] == '\0') {
            return (api_result_t){.status = 400, .error = "missing ssid", .ok = false};
        }
        if (!json_extract_string_field(body, "password", password, sizeof(password))) {
            password[0] = '\0';
        }
    }

    if (!apply_ok) {
        return (api_result_t){.status = 409, .error = "uplink apply failed", .ok = false};
    }

    return (api_result_t){.status = 200, .error = NULL, .ok = true};
}

static api_result_t simulate_ota_post(const char *body, bool start_ok)
{
    if (!body || body[0] == '\0') {
        return (api_result_t){.status = 400, .error = "empty body", .ok = false};
    }

    char url[192] = {0};
    if (!json_extract_string_field(body, "url", url, sizeof(url))) {
        return (api_result_t){.status = 400, .error = "missing url", .ok = false};
    }
    if (url[0] == '\0') {
        return (api_result_t){.status = 400, .error = "invalid url", .ok = false};
    }
    if (!start_ok) {
        return (api_result_t){.status = 409, .error = "ota start failed", .ok = false};
    }
    return (api_result_t){.status = 200, .error = NULL, .ok = true};
}

static const char *VALID_STATUS_JSON =
    "{"
    "\"ts\":123456,"
    "\"self\":\"AA:BB:CC:DD:EE:FF\","
    "\"heapKb\":256,"
    "\"core0LoadPct\":37,"
    "\"latencyMs\":11,"
    "\"netIf\":\"usb_ncm (10.0.0.1)\","
    "\"buildLabel\":\"SRC\","
    "\"meshState\":\"Mesh OK\","
    "\"bpm\":null,"
    "\"fftBins\":[0.1,1.25,2.5],"
    "\"nodes\":["
      "{"
      "\"mac\":\"AA:BB:CC:DD:EE:FF\",\"role\":\"TX\",\"root\":true,\"layer\":0,\"rssi\":-30,"
      "\"children\":1,\"streaming\":true,\"parent\":null,\"uptime\":1000,\"stale\":false"
      "},"
      "{"
      "\"mac\":\"11:22:33:44:55:66\",\"role\":\"RX\",\"root\":false,\"layer\":1,\"rssi\":-50,"
      "\"children\":0,\"streaming\":false,\"parent\":\"AA:BB:CC:DD:EE:FF\",\"uptime\":900,\"stale\":false"
      "}"
    "],"
    "\"monitor\":[{\"seq\":1,\"line\":\"ok\"}],"
    "\"ota\":{\"enabled\":true,\"mode\":\"https\"},"
    "\"uplink\":{"
      "\"enabled\":true,\"configured\":true,\"rootApplied\":false,\"pendingApply\":true,"
      "\"ssid\":\"MeshNet\",\"lastError\":\"\",\"updatedMs\":99"
    "}"
    "}";

static const char *STATUS_WITH_ADDITIONAL_FIELDS =
    "{"
    "\"ts\":42,"
    "\"self\":\"AA:BB:CC:DD:EE:FF\","
    "\"heapKb\":128,"
    "\"core0LoadPct\":null,"
    "\"latencyMs\":5,"
    "\"netIf\":\"usb_ncm (10.0.0.1)\","
    "\"buildLabel\":\"OUT\","
    "\"meshState\":\"Mesh Degraded\","
    "\"bpm\":120,"
    "\"fftBins\":null,"
    "\"extraTop\":\"future\","
    "\"nodes\":["
      "{"
      "\"mac\":\"AA:BB:CC:DD:EE:FF\",\"role\":\"RX\",\"root\":false,\"layer\":1,\"rssi\":-60,"
      "\"children\":0,\"streaming\":false,\"parent\":null,\"uptime\":1,\"stale\":false,"
      "\"futureNodeFlag\":true"
      "}"
    "]"
    "}";

void test_status_required_fields_types_and_invariants(void)
{
    TEST_ASSERT_TRUE(validate_status_contract(VALID_STATUS_JSON));
}

void test_status_allows_additional_fields(void)
{
    TEST_ASSERT_TRUE(validate_status_contract(STATUS_WITH_ADDITIONAL_FIELDS));
}

void test_status_rejects_duplicate_node_mac(void)
{
    const char *json =
        "{"
        "\"ts\":1,\"self\":\"AA:BB:CC:DD:EE:FF\",\"heapKb\":1,\"core0LoadPct\":10,"
        "\"latencyMs\":1,\"netIf\":\"n\",\"buildLabel\":\"SRC\",\"meshState\":\"Mesh OK\","
        "\"bpm\":null,\"fftBins\":null,"
        "\"nodes\":["
        "{\"mac\":\"AA:BB:CC:DD:EE:FF\",\"role\":\"TX\",\"root\":true,\"layer\":0,\"rssi\":-1,\"children\":0,\"streaming\":true,\"parent\":null,\"uptime\":1,\"stale\":false},"
        "{\"mac\":\"AA:BB:CC:DD:EE:FF\",\"role\":\"RX\",\"root\":false,\"layer\":1,\"rssi\":-2,\"children\":0,\"streaming\":false,\"parent\":\"AA:BB:CC:DD:EE:FF\",\"uptime\":2,\"stale\":false}"
        "]"
        "}";
    TEST_ASSERT_FALSE(validate_status_contract(json));
}

void test_status_rejects_root_node_nonzero_layer(void)
{
    const char *json =
        "{"
        "\"ts\":1,\"self\":\"AA:BB:CC:DD:EE:FF\",\"heapKb\":1,\"core0LoadPct\":10,"
        "\"latencyMs\":1,\"netIf\":\"n\",\"buildLabel\":\"SRC\",\"meshState\":\"Mesh OK\","
        "\"bpm\":null,\"fftBins\":null,"
        "\"nodes\":["
        "{\"mac\":\"AA:BB:CC:DD:EE:FF\",\"role\":\"TX\",\"root\":true,\"layer\":2,\"rssi\":-1,\"children\":0,\"streaming\":true,\"parent\":null,\"uptime\":1,\"stale\":false}"
        "]"
        "}";
    TEST_ASSERT_FALSE(validate_status_contract(json));
}

void test_status_rejects_invalid_parent_mac(void)
{
    const char *json =
        "{"
        "\"ts\":1,\"self\":\"AA:BB:CC:DD:EE:FF\",\"heapKb\":1,\"core0LoadPct\":10,"
        "\"latencyMs\":1,\"netIf\":\"n\",\"buildLabel\":\"SRC\",\"meshState\":\"Mesh OK\","
        "\"bpm\":null,\"fftBins\":null,"
        "\"nodes\":["
        "{\"mac\":\"AA:BB:CC:DD:EE:FF\",\"role\":\"TX\",\"root\":false,\"layer\":1,\"rssi\":-1,\"children\":0,\"streaming\":true,\"parent\":\"BAD-MAC\",\"uptime\":1,\"stale\":false}"
        "]"
        "}";
    TEST_ASSERT_FALSE(validate_status_contract(json));
}

void test_ws_payload_matches_status_contract(void)
{
    TEST_ASSERT_TRUE(validate_status_contract(VALID_STATUS_JSON));
}

void test_uplink_get_required_fields_contract(void)
{
    const char *json =
        "{"
        "\"enabled\":true,\"configured\":true,\"rootApplied\":false,\"pendingApply\":true,"
        "\"ssid\":\"MeshNet\",\"lastError\":\"\",\"updatedMs\":88"
        "}";
    TEST_ASSERT_TRUE(validate_uplink_contract(json));
}

void test_uplink_get_allows_additional_fields(void)
{
    const char *json =
        "{"
        "\"enabled\":false,\"configured\":false,\"rootApplied\":false,\"pendingApply\":false,"
        "\"ssid\":\"\",\"lastError\":\"none\",\"updatedMs\":1,\"future\":123"
        "}";
    TEST_ASSERT_TRUE(validate_uplink_contract(json));
}

void test_uplink_post_enable_valid(void)
{
    api_result_t res = simulate_uplink_post("{\"enabled\":true,\"ssid\":\"Mesh\",\"password\":\"pw\"}", true);
    TEST_ASSERT_EQUAL_INT(200, res.status);
    TEST_ASSERT_TRUE(res.ok);
}

void test_uplink_post_disable_valid(void)
{
    api_result_t res = simulate_uplink_post("{\"enabled\":false,\"ssid\":\"ignored\",\"password\":\"ignored\"}", true);
    TEST_ASSERT_EQUAL_INT(200, res.status);
    TEST_ASSERT_TRUE(res.ok);
}

void test_uplink_post_missing_enabled_400(void)
{
    api_result_t res = simulate_uplink_post("{\"ssid\":\"Mesh\"}", true);
    TEST_ASSERT_EQUAL_INT(400, res.status);
    TEST_ASSERT_EQUAL_STRING("missing enabled", res.error);
}

void test_uplink_post_enabled_missing_ssid_400(void)
{
    api_result_t res = simulate_uplink_post("{\"enabled\":true}", true);
    TEST_ASSERT_EQUAL_INT(400, res.status);
    TEST_ASSERT_EQUAL_STRING("missing ssid", res.error);
}

void test_uplink_post_extra_fields_ignored(void)
{
    api_result_t res = simulate_uplink_post("{\"enabled\":true,\"ssid\":\"Mesh\",\"extra\":{\"a\":1}}", true);
    TEST_ASSERT_EQUAL_INT(200, res.status);
    TEST_ASSERT_TRUE(res.ok);
}

void test_uplink_post_string_spacing_compat_limit_documented(void)
{
    api_result_t res = simulate_uplink_post("{\"enabled\":true,\"ssid\": \"Mesh\"}", true);
    TEST_ASSERT_EQUAL_INT(400, res.status);
    TEST_ASSERT_EQUAL_STRING("missing ssid", res.error);
}

void test_uplink_post_apply_failure_409(void)
{
    api_result_t res = simulate_uplink_post("{\"enabled\":true,\"ssid\":\"Mesh\"}", false);
    TEST_ASSERT_EQUAL_INT(409, res.status);
    TEST_ASSERT_EQUAL_STRING("uplink apply failed", res.error);
}

void test_ota_get_required_fields_contract(void)
{
    const char *json =
        "{"
        "\"enabled\":true,\"inProgress\":false,\"phase\":\"idle\","
        "\"lastOk\":false,\"lastErr\":0,\"lastUrl\":\"https://example.com/fw.bin\""
        "}";
    TEST_ASSERT_TRUE(validate_ota_contract(json));
}

void test_ota_get_allows_additional_fields(void)
{
    const char *json =
        "{"
        "\"enabled\":true,\"inProgress\":true,\"phase\":\"downloading\","
        "\"lastOk\":false,\"lastErr\":-1,\"lastUrl\":\"https://example.com/fw.bin\","
        "\"extra\":\"future\""
        "}";
    TEST_ASSERT_TRUE(validate_ota_contract(json));
}

void test_ota_post_valid_https_url(void)
{
    api_result_t res = simulate_ota_post("{\"url\":\"https://example.com/fw.bin\"}", true);
    TEST_ASSERT_EQUAL_INT(200, res.status);
    TEST_ASSERT_TRUE(res.ok);
}

void test_ota_post_missing_url_400(void)
{
    api_result_t res = simulate_ota_post("{\"enabled\":true}", true);
    TEST_ASSERT_EQUAL_INT(400, res.status);
    TEST_ASSERT_EQUAL_STRING("missing url", res.error);
}

void test_ota_post_empty_url_400(void)
{
    api_result_t res = simulate_ota_post("{\"url\":\"\"}", true);
    TEST_ASSERT_EQUAL_INT(400, res.status);
    TEST_ASSERT_EQUAL_STRING("invalid url", res.error);
}

void test_ota_post_invalid_scheme_maps_to_409_start_failure(void)
{
    api_result_t res = simulate_ota_post("{\"url\":\"http://example.com/fw.bin\"}", false);
    TEST_ASSERT_EQUAL_INT(409, res.status);
    TEST_ASSERT_EQUAL_STRING("ota start failed", res.error);
}

void test_ota_post_extra_fields_ignored(void)
{
    api_result_t res = simulate_ota_post("{\"url\":\"https://example.com/fw.bin\",\"extra\":123}", true);
    TEST_ASSERT_EQUAL_INT(200, res.status);
    TEST_ASSERT_TRUE(res.ok);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_status_required_fields_types_and_invariants);
    RUN_TEST(test_status_allows_additional_fields);
    RUN_TEST(test_status_rejects_duplicate_node_mac);
    RUN_TEST(test_status_rejects_root_node_nonzero_layer);
    RUN_TEST(test_status_rejects_invalid_parent_mac);
    RUN_TEST(test_ws_payload_matches_status_contract);
    RUN_TEST(test_uplink_get_required_fields_contract);
    RUN_TEST(test_uplink_get_allows_additional_fields);
    RUN_TEST(test_uplink_post_enable_valid);
    RUN_TEST(test_uplink_post_disable_valid);
    RUN_TEST(test_uplink_post_missing_enabled_400);
    RUN_TEST(test_uplink_post_enabled_missing_ssid_400);
    RUN_TEST(test_uplink_post_extra_fields_ignored);
    RUN_TEST(test_uplink_post_string_spacing_compat_limit_documented);
    RUN_TEST(test_uplink_post_apply_failure_409);
    RUN_TEST(test_ota_get_required_fields_contract);
    RUN_TEST(test_ota_get_allows_additional_fields);
    RUN_TEST(test_ota_post_valid_https_url);
    RUN_TEST(test_ota_post_missing_url_400);
    RUN_TEST(test_ota_post_empty_url_400);
    RUN_TEST(test_ota_post_invalid_scheme_maps_to_409_start_failure);
    RUN_TEST(test_ota_post_extra_fields_ignored);
    return UNITY_END();
}
