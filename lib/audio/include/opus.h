#ifndef OPUS_H
#define OPUS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct OpusEncoder OpusEncoder;
typedef struct OpusDecoder OpusDecoder;

typedef int16_t opus_int16;
typedef int32_t opus_int32;

#define OPUS_OK 0
#define OPUS_BAD_ARG -1
#define OPUS_INTERNAL_ERROR -3

#define OPUS_APPLICATION_AUDIO 2049
#define OPUS_SIGNAL_MUSIC 3002

// Control request placeholders for compatibility with the libopus API usage in adf_pipeline.c.
#define OPUS_SET_BITRATE(x) (4002)
#define OPUS_SET_COMPLEXITY(x) (4010)
#define OPUS_SET_SIGNAL(x) (4024)
#define OPUS_SET_VBR(x) (4006)
#define OPUS_SET_VBR_CONSTRAINT(x) (4020)

OpusEncoder *opus_encoder_create(opus_int32 Fs, int channels, int application, int *error);
void opus_encoder_destroy(OpusEncoder *st);
int opus_encoder_ctl(OpusEncoder *st, int request, ...);
int opus_encode(OpusEncoder *st, const opus_int16 *pcm, int frame_size, unsigned char *data, opus_int32 max_data_bytes);

OpusDecoder *opus_decoder_create(opus_int32 Fs, int channels, int *error);
void opus_decoder_destroy(OpusDecoder *st);
int opus_decode(OpusDecoder *st, const unsigned char *data, opus_int32 len, opus_int16 *pcm, int frame_size, int decode_fec);
const char *opus_strerror(int error);

#ifdef __cplusplus
}
#endif

#endif  // OPUS_H
