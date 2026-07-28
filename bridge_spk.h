/*
 * Shared speaker ESP-NOW wire format (Node A <-> Node B).
 * Mic path unchanged: 200 B IMA ADPCM @ 16 kHz.
 */
#ifndef BRIDGE_SPK_H
#define BRIDGE_SPK_H

#include <stdint.h>

#define BRIDGE_SPK_SAMPLE_RATE_HZ   48000
#define BRIDGE_SPK_FRAME_SAMPLES    480     /* 10 ms @ 48 kHz mono */
#define BRIDGE_SPK_PCM_BYTES        (BRIDGE_SPK_FRAME_SAMPLES * (int)sizeof(int16_t))

/* Legacy IMA ADPCM (fixed length). */
#define BRIDGE_SPK_ADPCM_PKT_BYTES  240

/* ESP-NOW max payload; v2 Opus packets are variable len <= this. */
#define BRIDGE_SPK_MAX_PKT_BYTES    250

#define BRIDGE_SPK_OPUS_MAGIC       0x4F
#define BRIDGE_SPK_OPUS_HDR_BYTES   2       /* magic + opus_payload_len */

#define BRIDGE_SPK_OPUS_BITRATE     128000
#define BRIDGE_SPK_OPUS_COMPLEXITY_ENC  2
#define BRIDGE_SPK_OPUS_COMPLEXITY_DEC  2

static inline int bridge_spk_is_adpcm_pkt(int len)
{
    return len == BRIDGE_SPK_ADPCM_PKT_BYTES;
}

static inline int bridge_spk_is_opus_pkt(const uint8_t *data, int len)
{
    if (len < BRIDGE_SPK_OPUS_HDR_BYTES + 1 || len > BRIDGE_SPK_MAX_PKT_BYTES) {
        return 0;
    }
    if (data[0] != BRIDGE_SPK_OPUS_MAGIC) {
        return 0;
    }
    uint8_t opus_len = data[1];
    return (int)opus_len > 0 && (BRIDGE_SPK_OPUS_HDR_BYTES + (int)opus_len) == len;
}

#endif /* BRIDGE_SPK_H */
