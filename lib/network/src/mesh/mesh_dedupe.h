#pragma once

#include <stdbool.h>
#include <stdint.h>

bool mesh_dedupe_is_duplicate(uint8_t stream_id, uint16_t seq);
void mesh_dedupe_mark_seen(uint8_t stream_id, uint16_t seq);
void mesh_dedupe_reset(void);
