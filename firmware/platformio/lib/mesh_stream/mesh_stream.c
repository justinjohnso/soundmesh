#include "mesh_stream.h"
#include "audio_element.h"
#include "audio_mem.h"
#include "audio_error.h"
#include "esp_log.h"
#include "sdkconfig.h"
#include <string.h>

static const char *TAG = "mesh_stream";

#define MESH_HDR_MAGIC 0x4D534831u /* 'MSH1' */

// TODO: implement minimal framing + esp_mesh_send/recv with queue & jitter buffer.
// This is a stub that compiles; full implementation follows after project scaffolding.

typedef struct {
    audio_element_handle_t el;
    mesh_stream_cfg_t cfg;
} mesh_stream_t;

static esp_err_t _open(audio_element_handle_t self) {
    ESP_LOGI(TAG, "open: %s", ((mesh_stream_t*)audio_element_getdata(self))->cfg.is_writer ? "writer" : "reader");
    return ESP_OK;
}

static esp_err_t _close(audio_element_handle_t self) {
    ESP_LOGI(TAG, "close");
    return ESP_OK;
}

static int _process_writer(audio_element_handle_t self, char *buf, int len) {
    // For now, just consume input to keep pipeline flowing.
    return len;
}

static int _process_reader(audio_element_handle_t self, char *buf, int len) {
    // For now, produce no data.
    return AEL_IO_DONE;
}

static int _process(audio_element_handle_t self, char *in, int in_len) {
    mesh_stream_t *h = (mesh_stream_t*)audio_element_getdata(self);
    return h->cfg.is_writer ? _process_writer(self, in, in_len) : _process_reader(self, in, in_len);
}

audio_element_handle_t mesh_stream_init(const mesh_stream_cfg_t *cfg) {
    mesh_stream_t *h = audio_calloc(1, sizeof(mesh_stream_t));
    AUDIO_MEM_CHECK(TAG, h, return NULL);
    h->cfg = *cfg;

    audio_element_cfg_t aecfg = DEFAULT_AUDIO_ELEMENT_CONFIG();
    aecfg.open = _open;
    aecfg.close = _close;
    aecfg.process = _process;
    aecfg.task_stack = 4 * 1024;
    aecfg.task_prio = 5;
    aecfg.tag = h->cfg.is_writer ? "mesh_writer" : "mesh_reader";

    h->el = audio_element_init(&aecfg);
    AUDIO_MEM_CHECK(TAG, h->el, { audio_free(h); return NULL; });
    audio_element_setdata(h->el, h);
    return h->el;
}
