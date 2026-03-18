#include "opus.h"

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

struct OpusEncoder {
    opus_int32 sample_rate;
    int channels;
};

struct OpusDecoder {
    opus_int32 sample_rate;
    int channels;
};

OpusEncoder *opus_encoder_create(opus_int32 Fs, int channels, int application, int *error) {
    (void)application;
    if (Fs <= 0 || channels <= 0) {
        if (error) {
            *error = OPUS_BAD_ARG;
        }
        return NULL;
    }

    OpusEncoder *enc = (OpusEncoder *)calloc(1, sizeof(OpusEncoder));
    if (!enc) {
        if (error) {
            *error = OPUS_INTERNAL_ERROR;
        }
        return NULL;
    }

    enc->sample_rate = Fs;
    enc->channels = channels;
    if (error) {
        *error = OPUS_OK;
    }
    return enc;
}

void opus_encoder_destroy(OpusEncoder *st) {
    free(st);
}

int opus_encoder_ctl(OpusEncoder *st, int request, ...) {
    (void)st;
    (void)request;
    return OPUS_OK;
}

int opus_encode(OpusEncoder *st, const opus_int16 *pcm, int frame_size, unsigned char *data, opus_int32 max_data_bytes) {
    if (!st || !pcm || !data || frame_size <= 0 || max_data_bytes <= 0) {
        return OPUS_BAD_ARG;
    }

    const int sample_count = frame_size * st->channels;
    const int bytes_needed = sample_count * (int)sizeof(opus_int16);
    const int bytes_to_copy = bytes_needed < max_data_bytes ? bytes_needed : max_data_bytes;
    memcpy(data, pcm, (size_t)bytes_to_copy);
    return bytes_to_copy;
}

OpusDecoder *opus_decoder_create(opus_int32 Fs, int channels, int *error) {
    if (Fs <= 0 || channels <= 0) {
        if (error) {
            *error = OPUS_BAD_ARG;
        }
        return NULL;
    }

    OpusDecoder *dec = (OpusDecoder *)calloc(1, sizeof(OpusDecoder));
    if (!dec) {
        if (error) {
            *error = OPUS_INTERNAL_ERROR;
        }
        return NULL;
    }

    dec->sample_rate = Fs;
    dec->channels = channels;
    if (error) {
        *error = OPUS_OK;
    }
    return dec;
}

void opus_decoder_destroy(OpusDecoder *st) {
    free(st);
}

int opus_decode(OpusDecoder *st, const unsigned char *data, opus_int32 len, opus_int16 *pcm, int frame_size, int decode_fec) {
    (void)decode_fec;
    if (!st || !data || !pcm || len <= 0 || frame_size <= 0) {
        return OPUS_BAD_ARG;
    }

    const int sample_count = frame_size * st->channels;
    const int max_bytes = sample_count * (int)sizeof(opus_int16);
    const int bytes_to_copy = len < max_bytes ? len : max_bytes;
    memcpy(pcm, data, (size_t)bytes_to_copy);
    if (bytes_to_copy < max_bytes) {
    memset((uint8_t *)pcm + bytes_to_copy, 0, (size_t)(max_bytes - bytes_to_copy));
    }
    return frame_size;
}

const char *opus_strerror(int error) {
    switch (error) {
        case OPUS_OK:
            return "OK";
        case OPUS_BAD_ARG:
            return "Bad argument";
        case OPUS_INTERNAL_ERROR:
            return "Internal error";
        default:
            return "Opus stub error";
    }
}
