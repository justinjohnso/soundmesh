#include "mesh/mesh_dedupe.h"
#include "config/build.h"
#include <esp_timer.h>

typedef struct {
    uint8_t stream_id;
    uint16_t seq;
    uint32_t timestamp_ms;
} recent_frame_t;

static recent_frame_t dedupe_cache[DEDUPE_CACHE_SIZE];
static int dedupe_index = 0;

bool mesh_dedupe_is_duplicate(uint8_t stream_id, uint16_t seq) {
    for (int i = 0; i < DEDUPE_CACHE_SIZE; i++) {
        if (dedupe_cache[i].stream_id == stream_id && dedupe_cache[i].seq == seq) {
            return true;
        }
    }
    return false;
}

void mesh_dedupe_mark_seen(uint8_t stream_id, uint16_t seq) {
    dedupe_cache[dedupe_index].stream_id = stream_id;
    dedupe_cache[dedupe_index].seq = seq;
    dedupe_cache[dedupe_index].timestamp_ms = esp_timer_get_time() / 1000;
    dedupe_index = (dedupe_index + 1) % DEDUPE_CACHE_SIZE;
}
