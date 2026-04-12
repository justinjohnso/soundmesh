#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

bool json_extract_string_field(const char *body, const char *field, char *out, size_t out_size);
bool json_extract_bool_field(const char *body, const char *field, bool *out);
bool json_extract_uint16_field(const char *body, const char *field, uint16_t *out);
bool json_extract_float_field(const char *body, const char *field, float *out);
bool json_extract_int_field(const char *body, const char *field, int *out);
bool json_extract_array_field_span(
    const char *body, const char *field, const char **start_out, const char **end_out);
bool json_extract_next_array_object_span(const char *array_start,
                                         const char *array_end,
                                         const char **cursor_io,
                                         const char **obj_start_out,
                                         const char **obj_end_out);
