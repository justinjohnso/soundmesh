#include "control/json_extract.h"

#include <stdio.h>
#include <string.h>

static bool build_json_string_key(const char *field, char *key, size_t key_size) {
    if (!field || !key || key_size == 0) {
        return false;
    }
    int written = snprintf(key, key_size, "\"%s\":\"", field);
    return written > 0 && (size_t)written < key_size;
}

static bool build_json_bool_key(const char *field, char *key, size_t key_size) {
    if (!field || !key || key_size == 0) {
        return false;
    }
    int written = snprintf(key, key_size, "\"%s\":", field);
    return written > 0 && (size_t)written < key_size;
}

bool json_extract_string_field(const char *body, const char *field, char *out, size_t out_size) {
    if (!body || !field || !out || out_size == 0) {
        return false;
    }

    char key[64];
    if (!build_json_string_key(field, key, sizeof(key))) {
        return false;
    }

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
    if (!build_json_bool_key(field, key, sizeof(key))) {
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
