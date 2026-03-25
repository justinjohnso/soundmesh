#include "mesh/mesh_dedupe.h"
#include "config/build.h"
#include <esp_timer.h>
#include <string.h>

typedef struct {
    uint8_t stream_id;
    uint16_t seq;
    uint32_t timestamp_ms;
} recent_frame_t;

static recent_frame_t dedupe_cache[DEDUPE_CACHE_SIZE];
static int dedupe_index = 0;

bool mesh_dedupe_is_duplicate(uint8_t stream_id, uint16_t seq) {
    const int start = dedupe_index;
    for (int offset = 0; offset < DEDUPE_CACHE_SIZE; offset++) {
        int idx = start - 1 - offset;
        if (idx < 0) {
            idx += DEDUPE_CACHE_SIZE;
        }
        if (dedupe_cache[idx].stream_id == stream_id && dedupe_cache[idx].seq == seq) {
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

void mesh_dedupe_reset(void) {
    memset(dedupe_cache, 0, sizeof(dedupe_cache));
    dedupe_index = 0;
}
