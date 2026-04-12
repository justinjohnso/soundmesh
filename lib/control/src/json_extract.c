#include "control/json_extract.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Builds a JSON key pattern like `"fieldName":` for non-string value lookup */
static bool build_json_field_key(const char *field, char *key, size_t key_size) {
    if (!field || !key || key_size == 0) {
        return false;
    }
    int written = snprintf(key, key_size, "\"%s\":", field);
    return written > 0 && (size_t)written < key_size;
}

static const char *skip_json_ws(const char *p) {
    while (p && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) {
        p++;
    }
    return p;
}

static const char *json_extract_find_matching_delim(const char *start, char open, char close) {
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

bool json_extract_string_field(const char *body, const char *field, char *out, size_t out_size) {
    if (!body || !field || !out || out_size == 0) {
        return false;
    }

    /* String values need the opening quote included in the search key */
    char key[66];
    if (!field || strlen(field) + 4 >= sizeof(key)) {
        return false;
    }
    snprintf(key, sizeof(key), "\"%s\":\"", field);

    const char *start = strstr(body, key);
    if (!start) {
        return false;
    }
    start += strlen(key);

    const char *end = strchr(start, '"');
    if (!end) {
        return false;
    }

    size_t len = (size_t)(end - start);
    if (len >= out_size) {
        return false;
    }

    memcpy(out, start, len);
    out[len] = '\0';
    return true;
}

bool json_extract_bool_field(const char *body, const char *field, bool *out) {
    if (!body || !field || !out) {
        return false;
    }

    char key[64];
    if (!build_json_field_key(field, key, sizeof(key))) {
        return false;
    }

    const char *start = strstr(body, key);
    if (!start) {
        return false;
    }
    start += strlen(key);
    while (*start == ' ' || *start == '\t') {
        start++;
    }

    if (strncmp(start, "true", 4) == 0) {
        *out = true;
        return true;
    }
    if (strncmp(start, "false", 5) == 0) {
        *out = false;
        return true;
    }
    return false;
}

bool json_extract_uint16_field(const char *body, const char *field, uint16_t *out) {
    if (!body || !field || !out) {
        return false;
    }

    char key[64];
    if (!build_json_field_key(field, key, sizeof(key))) {
        return false;
    }

    const char *start = strstr(body, key);
    if (!start) {
        return false;
    }
    start += strlen(key);
    while (*start == ' ' || *start == '\t') {
        start++;
    }
    if (!isdigit((unsigned char)*start)) {
        return false;
    }

    uint32_t value = 0;
    while (isdigit((unsigned char)*start)) {
        value = (value * 10u) + (uint32_t)(*start - '0');
        if (value > 65535u) {
            return false;
        }
        start++;
    }
    *out = (uint16_t)value;
    return true;
}

bool json_extract_float_field(const char *body, const char *field, float *out)
{
    if (!body || !field || !out) {
        return false;
    }

    char key[64];
    if (!build_json_field_key(field, key, sizeof(key))) {
        return false;
    }

    const char *start = strstr(body, key);
    if (!start) {
        return false;
    }
    start += strlen(key);
    while (*start == ' ' || *start == '\t') {
        start++;
    }

    char *end;
    float val = strtof(start, &end);
    if (end == start) {
        return false;
    }
    *out = val;
    return true;
}

bool json_extract_int_field(const char *body, const char *field, int *out)
{
    if (!body || !field || !out) {
        return false;
    }

    char key[64];
    if (!build_json_field_key(field, key, sizeof(key))) {
        return false;
    }

    const char *start = strstr(body, key);
    if (!start) {
        return false;
    }
    start += strlen(key);
    while (*start == ' ' || *start == '\t') {
        start++;
    }

    char *end;
    long val = strtol(start, &end, 10);
    if (end == start) {
        return false;
    }
    *out = (int)val;
    return true;
}

bool json_extract_array_field_span(
    const char *body, const char *field, const char **start_out, const char **end_out)
{
    if (!body || !field || !start_out || !end_out) {
        return false;
    }

    char key[64];
    if (!build_json_field_key(field, key, sizeof(key))) {
        return false;
    }

    const char *start = strstr(body, key);
    if (!start) {
        return false;
    }
    start += strlen(key);
    start = skip_json_ws(start);
    if (!start || *start != '[') {
        return false;
    }

    const char *end = json_extract_find_matching_delim(start, '[', ']');
    if (!end) {
        return false;
    }

    *start_out = start;
    *end_out = end;
    return true;
}

bool json_extract_next_array_object_span(const char *array_start,
                                         const char *array_end,
                                         const char **cursor_io,
                                         const char **obj_start_out,
                                         const char **obj_end_out)
{
    if (!array_start || !array_end || !cursor_io || !obj_start_out || !obj_end_out) {
        return false;
    }
    if (*array_start != '[' || *array_end != ']' || array_end < array_start) {
        return false;
    }

    const char *cursor = *cursor_io ? *cursor_io : array_start + 1;
    cursor = skip_json_ws(cursor);
    while (cursor < array_end && *cursor == ',') {
        cursor++;
        cursor = skip_json_ws(cursor);
    }

    if (cursor >= array_end) {
        *cursor_io = array_end;
        *obj_start_out = NULL;
        *obj_end_out = NULL;
        return true;
    }
    if (*cursor != '{') {
        return false;
    }

    const char *end = json_extract_find_matching_delim(cursor, '{', '}');
    if (!end || end > array_end) {
        return false;
    }

    *obj_start_out = cursor;
    *obj_end_out = end;
    *cursor_io = end + 1;
    return true;
}
