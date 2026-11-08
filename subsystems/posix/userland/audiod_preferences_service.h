#pragma once
#include "audiod_preferences_io.h"

static struct savanxp_audio_preferences g_preferences;
static int g_preferences_enabled, g_preferences_load_failed, g_preferences_error;
static uint32_t g_preferences_master = 100, g_preferences_muted;
static uint64_t g_preferences_generation, g_preferences_saved, g_preferences_child_generation;
static uint64_t g_preferences_dirty_ms, g_preferences_retry_ms, g_preferences_observe_ms, g_preferences_failed;
static long g_preferences_child = -1;
static int g_preferences_command = -1, g_preferences_reply = -1, g_preferences_saving;
static void audiod_copy_name(char*, const char*);
static uint32_t audiod_inventory(struct savanxp_audio_endpoint*);
static int audiod_process(uint32_t pid, struct savanxp_process_info* process) {
    unsigned long index;
    for (index = 0; index < 1024; ++index) {
        if (proc_info(index,process) <= 0) return 0;
        if (process->pid == pid) return 1;
    }
    return 0;
}

static void audiod_preferences_changed(void) {
    if (!g_preferences_enabled) return;
    ++g_preferences_generation; g_preferences_dirty_ms = uptime_ms();
}
static void audiod_preferences_attach(struct audiod_stream* stream) {
    struct savanxp_audio_application_key key = {0};
    struct audiod_native_client* client;
    uint32_t slot, i;
    if (stream->port < SAVANXP_AUDIOD_NATIVE_FIRST_PORT ||
        stream->port >= SAVANXP_AUDIOD_NATIVE_FIRST_PORT+SAVANXP_AUDIOD_NATIVE_SLOTS) return;
    slot = stream->port-SAVANXP_AUDIOD_NATIVE_FIRST_PORT; client = &g_native[slot];
    if (!client->used || !client->identity_valid || !client->role[0]) {
        if (stream->key_valid) { stream->volume = 100; stream->muted = 0; }
        stream->key_valid = 0;
        memset(&stream->key,0,sizeof(stream->key));
        return;
    }
    key.uid = client->uid;
    audiod_copy_name(key.image,client->image); audiod_copy_name(key.role,client->role);
    if (stream->key_valid && savanxp_audio_key_equal(&stream->key,&key)) return;
    stream->key = key; stream->key_valid = 1; stream->volume = 100; stream->muted = 0;
    if (!g_preferences_enabled) return;
    for (i = 0; i < g_preferences.applications; ++i) {
        if (!savanxp_audio_key_equal(&key,&g_preferences.app[i].key)) continue;
        stream->volume = g_preferences.app[i].volume; stream->muted = g_preferences.app[i].muted; return;
    }
}
static int audiod_preferences_can_change(const struct audiod_stream* stream, uint32_t volume, uint32_t muted) {
    uint32_t i;
    if (!g_preferences_enabled || !stream->key_valid || (volume == 100 && !muted) ||
        g_preferences.applications < SAVANXP_AUDIO_PREFERENCE_APPS) return 1;
    for (i = 0; i < g_preferences.applications; ++i)
        if (savanxp_audio_key_equal(&stream->key,&g_preferences.app[i].key)) return 1;
    g_preferences_error = -SAVANXP_ENOSPC;
    return 0;
}
static void audiod_preferences_stream(struct audiod_stream* stream) {
    uint32_t i;
    struct savanxp_audio_application_preference* entry;
    if (!g_preferences_enabled || !stream->key_valid) return;
    for (i = 0; i < g_preferences.applications; ++i)
        if (savanxp_audio_key_equal(&stream->key,&g_preferences.app[i].key)) break;
    if (stream->volume == 100 && !stream->muted) {
        if (i == g_preferences.applications) return;
        --g_preferences.applications;
        g_preferences.app[i] = g_preferences.app[g_preferences.applications];
        memset(&g_preferences.app[g_preferences.applications],0,sizeof(g_preferences.app[0]));
    } else {
        if (i == SAVANXP_AUDIO_PREFERENCE_APPS) { g_preferences_error = -SAVANXP_ENOSPC; return; }
        entry = &g_preferences.app[i];
        if (i == g_preferences.applications) ++g_preferences.applications;
        if (savanxp_audio_key_equal(&entry->key,&stream->key) && entry->volume == stream->volume && entry->muted == stream->muted) return;
        entry->key = stream->key; entry->volume = stream->volume; entry->muted = stream->muted;
    }
    audiod_preferences_changed();
}
static void audiod_preferences_observe(uint64_t now, int force) {
    struct savanxp_audio_capture_state input;
    long volume, muted;
    int changed = 0;
    if (!g_preferences_enabled || (!force && now < g_preferences_observe_ms)) return;
    g_preferences_observe_ms = now+100;
    volume = audio_get_volume(g_audio_fd); muted = audio_get_muted(g_audio_fd);
    if (volume >= 0 && volume <= 100 && muted >= 0 && muted <= 1 &&
        ((uint32_t)volume != g_preferences_master || (uint32_t)muted != g_preferences_muted)) {
        g_preferences_master = (uint32_t)volume; g_preferences_muted = (uint32_t)muted; changed = 1;
    }
    if (savanxp_ioctl(g_audio_fd,AUDIO_IOC_CAPTURE_GET_STATE,(unsigned long)(uintptr_t)&input) == 0 &&
        (input.volume != g_preferences.input_volume || input.muted != g_preferences.input_muted)) {
        g_preferences.input_volume = input.volume; g_preferences.input_muted = input.muted; changed = 1;
    }
    if (changed) audiod_preferences_changed();
}
static void audiod_preferences_route(uint64_t id, uint32_t direction) {
    if (!g_preferences_enabled) return;
    if (direction == SAVANXP_AUDIO_PLAYBACK) { g_preferences.output = id; g_preferences.headphones = 0; }
    else g_preferences.input = id;
    audiod_preferences_changed();
}
static void audiod_preferences_policy(void) {
    if (!g_preferences_enabled) return;
    g_preferences.headphones = g_jack_policy.enabled;
    g_preferences.fallback = g_jack_policy.fallback_id;
    if (!g_jack_policy.enabled) {
        struct savanxp_audio_endpoint endpoints[SAVANXP_AUDIO_MAX_ENDPOINTS];
        uint32_t count = audiod_inventory(endpoints), i;
        for (i = 0; i < count; ++i)
            if ((endpoints[i].flags&(SAVANXP_AUDIO_PLAYBACK|SAVANXP_AUDIO_SELECTED)) ==
                (SAVANXP_AUDIO_PLAYBACK|SAVANXP_AUDIO_SELECTED)) g_preferences.output = endpoints[i].id;
    }
    audiod_preferences_changed();
}
static void audiod_preferences_load(void) {
    static struct savanxp_audio_preferences candidate;
    static char text[SAVANXP_AUDIO_PREFERENCE_TEXT+1];
    struct savanxp_audio_endpoint endpoints[SAVANXP_AUDIO_MAX_ENDPOINTS];
    struct savanxp_audio_endpoint_selection selection = {0};
    uint32_t count, i, volume, muted;
    long bytes;
    savanxp_audio_preferences_init(&g_preferences);
    if (!g_preferences_enabled) return;
    bytes = audiod_preference_read_file("audio-mixer.cfg",text,sizeof(text));
    if (bytes >= 0) {
        if (savanxp_audio_preferences_decode(&candidate,text,(size_t)bytes)) g_preferences = candidate;
        else { g_preferences_load_failed = 1; g_preferences_error = -SAVANXP_EINVAL; }
    } else if (bytes != -SAVANXP_ENOENT) { g_preferences_load_failed = 1; g_preferences_error = (int)bytes; }
    bytes = audiod_preference_read_file("audio.cfg",text,16);
    if (bytes >= 0 && savanxp_audio_master_decode(text,(size_t)bytes,&volume,&muted)) {
        (void)audio_set_volume(g_audio_fd,(int)volume); (void)audio_set_muted(g_audio_fd,(int)muted);
    } else if (bytes != -SAVANXP_ENOENT) {
        g_preferences_load_failed = 1; g_preferences_error = bytes < 0 ? (int)bytes : -SAVANXP_EINVAL;
    }
    count = audiod_inventory(endpoints);
    for (i = 0; i < count; ++i) {
        const struct savanxp_audio_endpoint* ep = &endpoints[i];
        if (!(ep->flags&SAVANXP_AUDIO_AVAILABLE)) continue;
        if (ep->id == g_preferences.output && (ep->flags&SAVANXP_AUDIO_PLAYBACK)) selection.direction = SAVANXP_AUDIO_PLAYBACK;
        else if (ep->id == g_preferences.input && (ep->flags&SAVANXP_AUDIO_CAPTURE)) selection.direction = SAVANXP_AUDIO_CAPTURE;
        else continue;
        selection.id = ep->id;
        if (savanxp_ioctl(g_audio_fd,AUDIO_IOC_SELECT_ENDPOINT,(unsigned long)(uintptr_t)&selection) < 0)
            eprintf("audiod: saved connector could not be restored\n");
    }
    (void)savanxp_ioctl(g_audio_fd,AUDIO_IOC_CAPTURE_SET_VOLUME,g_preferences.input_volume);
    (void)savanxp_ioctl(g_audio_fd,AUDIO_IOC_CAPTURE_SET_MUTED,g_preferences.input_muted);
    if (g_preferences.headphones) {
        count = audiod_inventory(endpoints);
        if (!savanxp_jack_policy_enable(&g_jack_policy,endpoints,count)) {
            for (i = 0; i < count; ++i)
                if (endpoints[i].id == g_preferences.fallback && endpoints[i].device_id == g_jack_policy.device_id &&
                    (endpoints[i].flags&(SAVANXP_AUDIO_PLAYBACK|SAVANXP_AUDIO_AVAILABLE|SAVANXP_AUDIO_HEADPHONES)) ==
                    (SAVANXP_AUDIO_PLAYBACK|SAVANXP_AUDIO_AVAILABLE)) g_jack_policy.fallback_id = g_preferences.fallback;
        }
    }
    bytes = audio_get_volume(g_audio_fd); if (bytes >= 0) g_preferences_master = (uint32_t)bytes;
    bytes = audio_get_muted(g_audio_fd); if (bytes >= 0) g_preferences_muted = (uint32_t)bytes;
    if (g_preferences_load_failed) eprintf("audiod: invalid or unreadable sound preferences; using available defaults\n");
}
static long audiod_preferences_reply(struct audiod_native_client* client, uint32_t request, int result) {
    struct savanxp_audiod_record record = {0};
    struct savanxp_audiod_preference_reply reply = {SAVANXP_AUDIOD_CONTROL_MAGIC,SAVANXP_AUDIOD_CONTROL_VERSION,
        SAVANXP_AUDIOD_PREFERENCE_REPLY,request,result,g_preferences_generation,g_preferences_saved,0,g_preferences_error};
    if (g_preferences_enabled) reply.flags |= SAVANXP_AUDIOD_PREFERENCE_ENABLED;
    if (g_preferences_generation != g_preferences_saved) reply.flags |= SAVANXP_AUDIOD_PREFERENCE_DIRTY;
    if (g_preferences_saving) reply.flags |= SAVANXP_AUDIOD_PREFERENCE_SAVING;
    if (g_preferences_load_failed) reply.flags |= SAVANXP_AUDIOD_PREFERENCE_LOAD_FAILED;
    record.magic = SAVANXP_AUDIOD_RECORD_MAGIC; record.length = sizeof(reply);
    memcpy(record.payload,&reply,sizeof(reply));
    return sxn_write(client->write_handle,&record,sizeof(record));
}
static int audiod_preferences_start_worker(void) {
    const char* argv[] = {"/bin/audiod","--save-preferences"};
    int command[2], reply[2];
    long result;
    result = savanxp_pipe(command); if (result < 0) return (int)result;
    result = savanxp_pipe(reply);
    if (result < 0) { (void)savanxp_close(command[0]); (void)savanxp_close(command[1]); return (int)result; }
    result = spawn_fd(argv[0],argv,2,command[0],reply[1]);
    (void)savanxp_close(command[0]); (void)savanxp_close(reply[1]);
    if (result < 0) { (void)savanxp_close(command[1]); (void)savanxp_close(reply[0]); return (int)result; }
    g_preferences_child = result; g_preferences_command = command[1]; g_preferences_reply = reply[0];
    (void)sxn_set_nonblocking(g_preferences_command,1); (void)sxn_set_nonblocking(g_preferences_reply,1);
    return 0;
}
static void audiod_preferences_stop_worker(void) {
    int status;
    if (g_preferences_command >= 0) (void)savanxp_close(g_preferences_command);
    if (g_preferences_reply >= 0) (void)savanxp_close(g_preferences_reply);
    g_preferences_command = g_preferences_reply = -1;
    if (g_preferences_child > 0) {
        struct savanxp_process_info process;
        if (audiod_process((uint32_t)g_preferences_child,&process) && process.state != SAVANXP_PROC_ZOMBIE)
            (void)savanxp_kill((int)g_preferences_child,9);
        (void)savanxp_waitpid((int)g_preferences_child,&status);
    }
    g_preferences_child = -1; g_preferences_saving = 0;
}
static int audiod_preferences_start_save(void) {
    static struct audiod_preference_save request;
    long result;
    if (g_preferences_child < 0) return -SAVANXP_EIO;
    request.magic = AUDIOD_PREFERENCE_SAVE_MAGIC;
    request.bytes = (uint32_t)savanxp_audio_preferences_encode(&g_preferences,request.text,sizeof(request.text));
    request.master_volume = g_preferences_master; request.master_muted = g_preferences_muted;
    if (!request.bytes) return -SAVANXP_EINVAL;
    result = savanxp_write(g_preferences_command,&request,16+request.bytes);
    if (result == 16+(long)request.bytes) {
        g_preferences_saving = 1; g_preferences_child_generation = g_preferences_generation; return 0;
    }
    if (result != -SAVANXP_EAGAIN) audiod_preferences_stop_worker();
    return result < 0 ? (int)result : -SAVANXP_EIO;
}
static void audiod_preferences_tick(uint64_t now) {
    uint32_t slot;
    audiod_preferences_observe(now,0);
    if (g_preferences_saving) {
        int32_t status = -1;
        long got = savanxp_read(g_preferences_reply,&status,sizeof(status));
        if (got != -SAVANXP_EAGAIN) {
            g_preferences_saving = 0;
            if (got != sizeof(status)) { status = SAVANXP_EIO; audiod_preferences_stop_worker(); }
            if (!status) {
                g_preferences_saved = g_preferences_child_generation;
                g_preferences_error = g_preferences_load_failed = 0; g_preferences_failed = 0;
            } else {
                g_preferences_error = status > 0 && status < 4096 ? -status : -SAVANXP_EIO;
                g_preferences_failed = g_preferences_child_generation; g_preferences_retry_ms = now+5000;
            }
        }
    }
    if (g_preferences_enabled && g_preferences_child < 0 && now >= g_preferences_retry_ms && !g_capture_users) {
        int idle = 1;
        for (slot = 0; slot < AUDIOD_MAX_STREAMS; ++slot) if (g_streams[slot].used) idle = 0;
        if (idle) {
            const int result = audiod_preferences_start_worker();
            // Only a durable save clears the previous failure. Merely
            // restarting a worker must not turn a failed flush into success.
            if (result < 0) { g_preferences_error = result; g_preferences_retry_ms = now+5000; }
        }
    }
    if (g_preferences_enabled && !g_preferences_saving && g_preferences_generation != g_preferences_saved &&
        now >= g_preferences_retry_ms && now-g_preferences_dirty_ms >= 250) {
        const int result = audiod_preferences_start_save();
        if (result < 0) { g_preferences_error = result; g_preferences_failed = g_preferences_generation; g_preferences_retry_ms = now+5000; }
    }
    for (slot = 0; slot < SAVANXP_AUDIOD_NATIVE_SLOTS; ++slot) {
        struct audiod_native_client* client = &g_native[slot];
        int result = 0;
        long sent;
        if (!client->used || !client->flush_id) continue;
        if (g_preferences_saved < client->flush_generation) {
            if (!g_preferences_failed || g_preferences_failed < client->flush_generation) continue;
            result = g_preferences_error;
        }
        sent = audiod_preferences_reply(client,client->flush_id,result);
        if (sent == SAVANXP_AUDIOD_RECORD_BYTES || sent != -SAVANXP_EAGAIN) client->flush_id = 0;
    }
}
static void audiod_handle_preferences(struct audiod_native_client* client, const unsigned char* data, size_t length) {
    struct savanxp_audiod_preference_request request;
    uint64_t now = uptime_ms();
    if (length != sizeof(request)) return;
    memcpy(&request,data,sizeof(request));
    if (request.version != SAVANXP_AUDIOD_CONTROL_VERSION || request.reserved || !request.request_id) return;
    audiod_preferences_observe(now,1);
    if (!g_preferences_enabled || request.kind == SAVANXP_AUDIOD_PREFERENCE_QUERY) {
        (void)audiod_preferences_reply(client,request.request_id,g_preferences_enabled ? 0 : -SAVANXP_ENOSYS); return;
    }
    if (client->flush_id) { (void)audiod_preferences_reply(client,request.request_id,-SAVANXP_EBUSY); return; }
    if (g_preferences_load_failed && g_preferences_generation == g_preferences_saved) audiod_preferences_changed();
    client->flush_id = request.request_id; client->flush_generation = g_preferences_generation;
    g_preferences_failed = 0; g_preferences_retry_ms = 0;
    g_preferences_dirty_ms = now > 250 ? now-250 : 0;
}
