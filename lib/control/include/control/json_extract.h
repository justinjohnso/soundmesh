#pragma once

#include <stdbool.h>
#include <stddef.h>

bool json_extract_string_field(const char *body, const char *field, char *out, size_t out_size);
bool json_extract_bool_field(const char *body, const char *field, bool *out);
