#pragma once
/* Nonblocking SUN recording. No file or POSIX dependency; periods are buffered
 * here so a caller may ask for any number of complete stereo frames. */
#include "sxn/sxnaudio.h"

struct sxnaudio_capture {
    int mode;
    int error;
    long device;
    long to_daemon;
    long from_daemon;
    uint32_t rate_hz, period_bytes;
    uint32_t buffered, offset, pending;
    uint64_t next_frame, dropped_frames;
    int16_t samples[2048];
};
static inline long sxnaudio_capture_info(long device, struct savanxp_audio_info* info) {
    long result = sxn_device_control(device,AUDIO_IOC_CAPTURE_GET_INFO,(unsigned long)(uintptr_t)info);
    if (result < 0) return result;
    if (info->channels != 2 || info->bits_per_sample != 16 || info->frame_bytes != 4 ||
        info->sample_rate_hz < 8000 || info->sample_rate_hz > 48000 || !info->period_bytes ||
        info->period_bytes > 4096 || info->period_bytes%4) return -SAVANXP_EIO;
    return 0;
}
static inline void sxnaudio_capture_close(struct sxnaudio_capture* stream) {
    if (!stream) return;
    if (stream->mode == SXNAUDIO_MODE_DAEMON && stream->to_daemon >= 0) {
        struct savanxp_audiod_capture_request request = {SAVANXP_AUDIOD_CONTROL_MAGIC,
            SAVANXP_AUDIOD_CONTROL_VERSION,SAVANXP_AUDIOD_CAPTURE_STOP,2,0};
        (void)sxnaudio_send_record_to(stream->to_daemon,&request,sizeof(request));
    }
    if (stream->mode == SXNAUDIO_MODE_DIRECT && stream->device >= 0)
        (void)sxn_device_control(stream->device,AUDIO_IOC_CAPTURE_STOP,0);
    if (stream->to_daemon >= 0) (void)sxn_close(stream->to_daemon);
    if (stream->from_daemon >= 0) (void)sxn_close(stream->from_daemon);
    if (stream->device >= 0) (void)sxn_close(stream->device);
    stream->mode = SXNAUDIO_MODE_CLOSED;
    stream->device = stream->to_daemon = stream->from_daemon = -1;
    stream->buffered = stream->offset = stream->pending = 0;
    stream->error = 0;
}
static inline long sxnaudio_capture_open(struct sxnaudio_capture* stream) {
    struct savanxp_audio_info info;
    long result;
    if (!stream) return -SAVANXP_EINVAL;
    stream->mode = SXNAUDIO_MODE_CLOSED;
    stream->error = 0;
    stream->device = stream->to_daemon = stream->from_daemon = -1;
    stream->rate_hz = stream->period_bytes = stream->buffered = stream->offset = stream->pending = 0;
    stream->next_frame = stream->dropped_frames = 0;
    stream->device = sxn_open_device("audio0",SXN_DEVICE_READ);
    if (stream->device < 0) { result = stream->device; stream->device = -1; return result; }
    result = sxnaudio_capture_info(stream->device,&info);
    if (result < 0) { sxnaudio_capture_close(stream); return result; }
    stream->rate_hz = info.sample_rate_hz; stream->period_bytes = info.period_bytes;
    {
        int32_t handles[2] = {-1,-1};
        if (sxn_port_connect(SAVANXP_AUDIOD_PORT_NAME,handles) == 0) {
            struct savanxp_audiod_capture_request request = {SAVANXP_AUDIOD_CONTROL_MAGIC,
                SAVANXP_AUDIOD_CONTROL_VERSION,SAVANXP_AUDIOD_CAPTURE_START,1,0};
            stream->to_daemon = handles[0]; stream->from_daemon = handles[1];
            result = sxn_set_nonblocking(handles[0],1);
            if (result >= 0) result = sxn_set_nonblocking(handles[1],1);
            if (result >= 0) result = sxnaudio_send_record_to(handles[0],&request,sizeof(request));
            if (result < 0) { sxnaudio_capture_close(stream); return result; }
            stream->mode = SXNAUDIO_MODE_DAEMON; stream->pending = 1;
            return 0;
        }
    }
    result = sxn_device_control(stream->device,AUDIO_IOC_CAPTURE_START,0);
    if (result < 0) { sxnaudio_capture_close(stream); return result; }
    stream->mode = SXNAUDIO_MODE_DIRECT;
    return 0;
}
static inline long sxnaudio_capture_fill(struct sxnaudio_capture* stream) {
    for (;;) {
        struct savanxp_audiod_record record;
        const struct savanxp_audiod_list_query* header = (const struct savanxp_audiod_list_query*)record.payload;
        long got = sxn_read(stream->from_daemon,&record,sizeof(record));
        uint32_t i;
        if (got == -SAVANXP_EAGAIN) return 0;
        if (got < 0) return got;
        if (got != sizeof(record) || record.magic != SAVANXP_AUDIOD_RECORD_MAGIC ||
            record.length < sizeof(*header) || record.length > SAVANXP_AUDIOD_RECORD_PAYLOAD ||
            header->magic != SAVANXP_AUDIOD_CONTROL_MAGIC || header->version != SAVANXP_AUDIOD_CONTROL_VERSION)
            return -SAVANXP_EIO;
        if (header->kind == SAVANXP_AUDIOD_CAPTURE_REPLY) {
            const struct savanxp_audiod_capture_reply* reply = (const struct savanxp_audiod_capture_reply*)record.payload;
            if (record.length != sizeof(*reply) || reply->reserved) return -SAVANXP_EIO;
            if (reply->result < 0) return reply->result;
            if (!stream->pending || reply->request_id != 1 || reply->channels != 2 || reply->bits != 16 ||
                reply->rate_hz != stream->rate_hz || reply->period_bytes != stream->period_bytes) return -SAVANXP_EIO;
            stream->pending = 0;
            continue;
        }
        if (header->kind == SAVANXP_AUDIOD_CAPTURE_PCM) {
            const struct savanxp_audiod_capture_pcm* pcm = (const struct savanxp_audiod_capture_pcm*)record.payload;
            unsigned char* destination = (unsigned char*)stream->samples;
            if (stream->pending || record.length < sizeof(*pcm) || pcm->reserved || !pcm->frames ||
                pcm->frames > SAVANXP_AUDIOD_CAPTURE_MAX_FRAMES || record.length != sizeof(*pcm)+pcm->frames*4 ||
                pcm->frame_index < stream->next_frame || pcm->frame_index > UINT64_MAX-pcm->frames) return -SAVANXP_EIO;
            stream->dropped_frames += pcm->frame_index-stream->next_frame;
            stream->next_frame = pcm->frame_index+pcm->frames;
            for (i = 0; i < pcm->frames*4; ++i) destination[i] = record.payload[sizeof(*pcm)+i];
            stream->buffered = pcm->frames; stream->offset = 0;
            return 1;
        }
        return -SAVANXP_EIO;
    }
}

/* Frames copied, zero if no period is ready, or negative errno. Never returns
 * successful silence as a substitute for a device error. Gain and mute are
 * applied by the kernel before these samples leave the device. */
static inline long sxnaudio_capture_read(struct sxnaudio_capture* stream, int16_t* frames, uint32_t count) {
    uint32_t copied = 0;
    if (!stream || stream->mode == SXNAUDIO_MODE_CLOSED || (!frames && count)) return -SAVANXP_EINVAL;
    if (stream->error) return stream->error;
    while (copied < count) {
        uint32_t available, take, i;
        if (stream->offset == stream->buffered) {
            if (stream->mode == SXNAUDIO_MODE_DAEMON) {
                long result = sxnaudio_capture_fill(stream);
                if (result < 0) stream->error = (int)result;
                if (result <= 0) return copied ? (long)copied : result;
            } else {
                struct savanxp_audio_capture_read request;
                long result;
                request.buffer = (uint64_t)(uintptr_t)stream->samples;
                request.byte_count = stream->period_bytes; request.reserved = 0;
                result = sxn_device_control(stream->device,AUDIO_IOC_CAPTURE_READ,(unsigned long)(uintptr_t)&request);
                if (result == -SAVANXP_EAGAIN) return copied;
                if (result < 0) { stream->error = (int)result; return copied ? (long)copied : result; }
                stream->offset = 0; stream->buffered = stream->period_bytes/4;
            }
        }
        available = stream->buffered-stream->offset;
        take = count-copied < available ? count-copied : available;
        for (i = 0; i < take*2; ++i) frames[copied*2+i] = stream->samples[stream->offset*2+i];
        copied += take; stream->offset += take;
    }
    return copied;
}
static inline long sxnaudio_capture_state(long device, struct savanxp_audio_capture_state* state) {
    long result = sxn_device_control(device,AUDIO_IOC_CAPTURE_GET_STATE,(unsigned long)(uintptr_t)state);
    if (result < 0) return result;
    return state->volume <= 100 && state->muted <= 1 && state->active <= 1 ? 0 : -SAVANXP_EIO;
}
static inline long sxnaudio_capture_volume(long device, uint32_t volume) {
    if (volume > 100) return -SAVANXP_EINVAL;
    return sxn_device_control(device,AUDIO_IOC_CAPTURE_SET_VOLUME,volume);
}
static inline long sxnaudio_capture_muted(long device, uint32_t muted) {
    if (muted > 1) return -SAVANXP_EINVAL;
    return sxn_device_control(device,AUDIO_IOC_CAPTURE_SET_MUTED,muted);
}
