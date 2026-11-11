/* Sound: the SUN/SxGUI control panel for audiod, laid out as the classic
 * Sounds and Audio Devices property sheet. Live controls do not claim PCM
 * ownership; recording replay uses a separate playback stream. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "sxn/sxngui.h"
#include "sxn/sxnaudio.h"
#include "sxn/sxncapture.h"
#include "sound.sunres.h"

/* Index order is paint and Tab order: each group box comes before the
 * controls drawn inside it. */
enum { TABS,
       /* Volume */
       DEVICE_NAME, VOLUME_GROUP, MASTER, MASTER_TEXT, MASTER_LEVEL, MASTER_MUTE,
       SPEAKER_GROUP, JACK_POLICY,
       /* Applications */
       LIST, APP_GROUP, SELECTED, APP_VOLUME, APP_TEXT, APP_LEVEL, APP_MUTE,
       /* Audio */
       PLAYBACK_GROUP, OUTPUT, RECORDING_GROUP, INPUT_SELECT, INPUT_VOLUME, INPUT_TEXT,
       INPUT_LEVEL, INPUT_MUTE,
       /* Voice */
       TEST_GROUP, VOICE_LEVEL, RECORD, PLAY_RECORDING, RECORD_STATUS,
       /* Hardware */
       HW_LIST, HW_GROUP, HW_DEVICE, HW_CONNECTOR, HW_STATUS,
       STATUS, OK, CANCEL, APPLY, WIDGETS };
enum { PAGE_VOLUME, PAGE_APPLICATIONS, PAGE_AUDIO, PAGE_VOICE, PAGE_HARDWARE, PAGE_ALL = -1 };
static const char* g_tab_labels[] = {"Volume", "Applications", "Audio", "Voice", "Hardware"};
enum { TAB_COUNT = sizeof(g_tab_labels)/sizeof(g_tab_labels[0]) };
static struct sxngui_app g_app;
static struct sxgui_widget g_widgets[WIDGETS];
static struct sxnaudio_control g_control = {-1,-1};
static struct savanxp_audiod_snapshot g_snapshot;
static long g_device = -1;
static uint32_t g_volume = 100, g_muted, g_selected;
static uint32_t g_sequence, g_pending, g_route_pending;
static uint64_t g_route_ms, g_route_error_until;
static struct savanxp_audiod_jack_policy_reply g_policy;
static uint32_t g_policy_pending;
static int g_policy_changing;
static uint64_t g_policy_ms, g_next_policy_ms;
static struct savanxp_audiod_preference_reply g_preferences;
static uint32_t g_preferences_pending;
static uint64_t g_preferences_ms, g_next_preferences_ms;
static struct savanxp_audio_endpoint g_outputs[SAVANXP_AUDIO_MAX_ENDPOINTS];
static uint32_t g_output_count;
static char g_output_rows[SAVANXP_AUDIO_MAX_ENDPOINTS][100];
static const char* g_output_ptrs[SAVANXP_AUDIO_MAX_ENDPOINTS];
static char g_route_error[100];
static struct savanxp_audio_endpoint g_inputs[SAVANXP_AUDIO_MAX_ENDPOINTS];
static char g_input_rows[SAVANXP_AUDIO_MAX_ENDPOINTS][100];
static const char* g_input_ptrs[SAVANXP_AUDIO_MAX_ENDPOINTS];
static uint32_t g_input_count, g_route_direction;
static uint64_t g_route_previous_id;
static struct savanxp_audio_capture_state g_input_state;
static int g_input_available, g_capture_online;
static struct sxnaudio_capture g_capture = {.device=-1,.to_daemon=-1,.from_daemon=-1};
static struct sxnaudio_stream g_record_output = {.device=-1,.to_daemon=-1,.from_daemon=-1};
enum { RECORD_IDLE, RECORDING, RECORD_READY, RECORD_PLAYING };
static int g_record_state;
static int16_t g_record_pcm[144000*2];
static uint32_t g_record_frames;
static uint64_t g_capture_retry_ms, g_capture_last_ms, g_record_started_ms, g_capture_drops, g_next_ui_ms;
static long g_replay_thread = -1, g_replay_stop = -1;
static int g_replay_result;
static unsigned char g_replay_stack[32768] __attribute__((aligned(16)));
static char g_input_text[32], g_record_text[100];

static uint64_t g_query_ms, g_retry_ms, g_device_retry_ms;
static int g_online;
static char g_master_text[32], g_selected_text[64], g_app_text[32], g_status[100];
static char g_rows[SAVANXP_AUDIOD_MAX_STREAMS][100];
static const char* g_row_ptrs[SAVANXP_AUDIOD_MAX_STREAMS];
static const struct sxgui_column g_columns[] = {
    {"Application", 186, 0}, {"Volume", 62, 0}, {"Muted", 54, 0}, {"Level", 56, 0}
};
/* Hardware tab: every connector audio0 reports, playback and capture. */
static struct savanxp_audio_endpoint g_hw[SAVANXP_AUDIO_MAX_ENDPOINTS];
static char g_hw_rows[SAVANXP_AUDIO_MAX_ENDPOINTS][120];
static const char* g_hw_ptrs[SAVANXP_AUDIO_MAX_ENDPOINTS];
static uint32_t g_hw_count;
static uint64_t g_hw_selected_id;
static const struct sxgui_column g_hw_columns[] = {{"Name", 270, 0}, {"Type", 96, 0}};
static char g_device_text[100], g_hw_device[120], g_hw_connector[120], g_hw_status[120];
/* Device levels as they were when the sheet opened, which Cancel restores. */
static uint32_t g_initial_volume, g_initial_muted, g_initial_input_volume, g_initial_input_muted;
static int g_initial_master_known, g_initial_input_known;

static struct savanxp_audiod_stream_state* selected(void) {
    uint32_t i;
    for (i = 0; i < g_snapshot.count; ++i)
        if (g_snapshot.streams[i].id == g_selected) return &g_snapshot.streams[i];
    return 0;
}
static int peak(uint16_t left, uint16_t right) {
    return (int)((uint32_t)(left > right ? left : right) * 100 / 32768);
}
static void refresh_outputs(void) {
    uint32_t index;
    int active = -1, input_active = -1;
    struct savanxp_audio_endpoint endpoint;
    int hw_active = -1;
    g_output_count = g_input_count = g_hw_count = 0;
    if (g_device >= 0) {
        for (index = 0; index < SAVANXP_AUDIO_MAX_ENDPOINTS; ++index) {
            if (sxnaudio_endpoint_get(g_device,index,&endpoint) < 0) break;
            const char* presence = !(endpoint.flags & SAVANXP_AUDIO_AVAILABLE) ? " (unavailable)" :
                !(endpoint.flags & SAVANXP_AUDIO_JACK) ? "" :
                !(endpoint.flags & SAVANXP_AUDIO_PRESENCE_CAPABLE) ? " (no detection)" :
                !(endpoint.flags & SAVANXP_AUDIO_PRESENCE_VALID) ? " (unknown)" :
                (endpoint.flags & SAVANXP_AUDIO_CONNECTED) ? " (connected)" : " (unplugged)";
            g_hw[g_hw_count] = endpoint;
            (void)snprintf(g_hw_rows[g_hw_count],sizeof(g_hw_rows[0]),"%s - %s\t%s",endpoint.device_name,endpoint.name,
                (endpoint.flags & SAVANXP_AUDIO_CAPTURE) ? "Recording" :
                (endpoint.flags & SAVANXP_AUDIO_PLAYBACK) ? "Playback" : "Other");
            g_hw_ptrs[g_hw_count] = g_hw_rows[g_hw_count];
            if (endpoint.id == g_hw_selected_id) hw_active = (int)g_hw_count;
            ++g_hw_count;
            if (endpoint.flags & SAVANXP_AUDIO_CAPTURE) {
                g_inputs[g_input_count] = endpoint;
                (void)snprintf(g_input_rows[g_input_count],sizeof(g_input_rows[0]),"%s - %s%s",
                    endpoint.device_name,endpoint.name,presence);
                g_input_ptrs[g_input_count] = g_input_rows[g_input_count];
                if (endpoint.flags & SAVANXP_AUDIO_SELECTED) input_active = (int)g_input_count;
                ++g_input_count;
                continue;
            }
            if (!(endpoint.flags & SAVANXP_AUDIO_PLAYBACK)) continue;
            g_outputs[g_output_count] = endpoint;
            (void)snprintf(g_output_rows[g_output_count],sizeof(g_output_rows[0]),"%s - %s%s",
                           endpoint.device_name,endpoint.name,presence);
            g_output_ptrs[g_output_count] = g_output_rows[g_output_count];
            if (endpoint.flags & SAVANXP_AUDIO_SELECTED) active = (int)g_output_count;
            ++g_output_count;
        }
    }
    /* The Volume tab names the device behind the effective output. */
    if (g_device < 0) (void)snprintf(g_device_text,sizeof(g_device_text),"No audio device");
    else if (active >= 0) (void)snprintf(g_device_text,sizeof(g_device_text),"%s",g_outputs[active].device_name);
    else if (g_hw_count) (void)snprintf(g_device_text,sizeof(g_device_text),"%s",g_hw[0].device_name);
    else (void)snprintf(g_device_text,sizeof(g_device_text),"No playback device");
    if (hw_active < 0 && g_hw_count) { hw_active = 0; g_hw_selected_id = g_hw[0].id; }
    g_widgets[HW_LIST].item_count = (int)g_hw_count;
    g_widgets[HW_LIST].value = hw_active;
    if (hw_active >= 0) {
        const struct savanxp_audio_endpoint* item = &g_hw[hw_active];
        const uint32_t flags = item->flags;
        (void)snprintf(g_hw_device,sizeof(g_hw_device),"Device: %s",item->device_name);
        (void)snprintf(g_hw_connector,sizeof(g_hw_connector),"Connector: %s%s",item->name,
            (flags & SAVANXP_AUDIO_SELECTED) ? " (default)" : "");
        (void)snprintf(g_hw_status,sizeof(g_hw_status),"Status: %s",
            !(flags & SAVANXP_AUDIO_AVAILABLE) ? "Unavailable" :
            !(flags & SAVANXP_AUDIO_JACK) ? "Working properly" :
            !(flags & SAVANXP_AUDIO_PRESENCE_CAPABLE) ? "Jack without plug detection" :
            !(flags & SAVANXP_AUDIO_PRESENCE_VALID) ? "Jack state unknown" :
            (flags & SAVANXP_AUDIO_CONNECTED) ? "Plugged in" : "Unplugged");
    } else {
        (void)snprintf(g_hw_device,sizeof(g_hw_device),"Device: %s",g_device < 0 ? "None" : "No connectors reported");
        g_hw_connector[0] = g_hw_status[0] = 0;
    }
    g_widgets[INPUT_SELECT].item_count = (int)g_input_count;
    g_widgets[INPUT_SELECT].value = input_active;
    if (g_input_count && g_online && !g_route_pending) g_widgets[INPUT_SELECT].flags &= ~SXGUI_FLAG_DISABLED;
    else g_widgets[INPUT_SELECT].flags |= SXGUI_FLAG_DISABLED;
    g_input_available = g_device >= 0 && g_input_count && sxnaudio_capture_state(g_device,&g_input_state) == 0;
    if (g_input_available) {
        g_widgets[INPUT_VOLUME].flags &= ~SXGUI_FLAG_DISABLED; g_widgets[INPUT_MUTE].flags &= ~SXGUI_FLAG_DISABLED;
        if (g_app.ui.capture_index != INPUT_VOLUME) g_widgets[INPUT_VOLUME].value = (int)g_input_state.volume;
        if (!g_widgets[INPUT_MUTE].pressed) g_widgets[INPUT_MUTE].value = (int)g_input_state.muted;
        g_widgets[INPUT_LEVEL].value = peak(g_input_state.peak_left,g_input_state.peak_right);
        (void)snprintf(g_input_text,sizeof(g_input_text),"%u%%",g_input_state.volume);
        if (!g_initial_input_known) {
            g_initial_input_volume = g_input_state.volume; g_initial_input_muted = g_input_state.muted;
            g_initial_input_known = 1;
        }
    } else {
        g_widgets[INPUT_VOLUME].flags |= SXGUI_FLAG_DISABLED; g_widgets[INPUT_MUTE].flags |= SXGUI_FLAG_DISABLED;
        g_widgets[INPUT_LEVEL].value = 0; g_input_text[0] = 0;
        if (!g_input_count) (void)snprintf(g_record_text,sizeof(g_record_text),"No recording device available");
    }
    g_widgets[VOICE_LEVEL].value = g_widgets[INPUT_LEVEL].value;
    if (g_capture_online && g_record_state != RECORDING) g_widgets[RECORD].flags &= ~SXGUI_FLAG_DISABLED;
    else g_widgets[RECORD].flags |= SXGUI_FLAG_DISABLED;
    if (g_record_frames && g_record_state == RECORD_READY && active >= 0)
        g_widgets[PLAY_RECORDING].flags &= ~SXGUI_FLAG_DISABLED;
    else g_widgets[PLAY_RECORDING].flags |= SXGUI_FLAG_DISABLED;
    g_widgets[JACK_POLICY].value = (int)g_policy.enabled;
    if (g_online && g_policy.supported && !g_policy_changing && !g_route_pending)
        g_widgets[JACK_POLICY].flags &= ~SXGUI_FLAG_DISABLED;
    else g_widgets[JACK_POLICY].flags |= SXGUI_FLAG_DISABLED;
    g_widgets[OUTPUT].item_count = (int)g_output_count;
    g_widgets[OUTPUT].value = active;
    if (g_output_count && g_online && !g_route_pending) g_widgets[OUTPUT].flags &= ~SXGUI_FLAG_DISABLED;
    else g_widgets[OUTPUT].flags |= SXGUI_FLAG_DISABLED;
}

static void refresh_controls(void) {
    struct savanxp_audiod_stream_state* stream = selected();
    uint32_t i;
    int selected_row = -1;
    int dragging = g_app.ui.capture_index == APP_VOLUME;
    if (!stream && !dragging) {
        g_selected = g_snapshot.count ? g_snapshot.streams[0].id : 0;
        stream = selected();
    }
    for (i = 0; i < g_snapshot.count; ++i) {
        const struct savanxp_audiod_stream_state* item = &g_snapshot.streams[i];
        (void)snprintf(g_rows[i], sizeof(g_rows[i]), "%s\t%u%%\t%s\t%d%%",
            item->name[0] ? item->name : "Unnamed application", item->volume,
            item->muted ? "Yes" : "No", peak(item->peak_left,item->peak_right));
        g_row_ptrs[i] = g_rows[i];
        if (item->id == g_selected) selected_row = (int)i;
    }
    g_widgets[LIST].item_count = (int)g_snapshot.count;
    g_widgets[LIST].value = selected_row;
    for (i = APP_VOLUME; i <= APP_MUTE; ++i) {
        if (stream && g_online) g_widgets[i].flags &= ~SXGUI_FLAG_DISABLED;
        else g_widgets[i].flags |= SXGUI_FLAG_DISABLED;
    }
    if (stream) {
        (void)snprintf(g_selected_text, sizeof(g_selected_text), "%s",
            stream->name[0] ? stream->name : "Unnamed application");
        if (!dragging) g_widgets[APP_VOLUME].value = stream->volume;
        if (!g_widgets[APP_MUTE].pressed) g_widgets[APP_MUTE].value = stream->muted;
        g_widgets[APP_LEVEL].value = peak(stream->peak_left,stream->peak_right);
        (void)snprintf(g_app_text,sizeof(g_app_text), "%u%%", stream->volume);
    } else {
        (void)snprintf(g_selected_text,sizeof(g_selected_text), "Select an application");
        g_widgets[APP_LEVEL].value = 0;
        g_app_text[0] = '\0';
    }
    if (g_app.ui.capture_index != MASTER) g_widgets[MASTER].value = (int)g_volume;
    if (!g_widgets[MASTER_MUTE].pressed) g_widgets[MASTER_MUTE].value = (int)g_muted;
    g_widgets[MASTER_LEVEL].value = g_muted ? 0 : peak(g_snapshot.peak_left,g_snapshot.peak_right) * g_volume / 100;
    (void)snprintf(g_master_text,sizeof(g_master_text), "%u%%", g_volume);
}
static void stop_record_output(void) {
    if (g_replay_thread >= 0) {
        (void)sxn_event_set(g_replay_stop);
        // Join before closing the stream, modifying PCM or reusing the stack.
        (void)sxn_wait_one(g_replay_thread,-1);
        (void)sxn_close(g_replay_thread); g_replay_thread = -1;
    }
    if (g_replay_stop >= 0) { (void)sxn_close(g_replay_stop); g_replay_stop = -1; }
    if (g_record_output.mode != SXNAUDIO_MODE_CLOSED) sxnaudio_close(&g_record_output);
    if (g_record_state == RECORD_PLAYING) g_record_state = g_record_frames ? RECORD_READY : RECORD_IDLE;
}
static void replay_worker(void* unused) {
    int16_t silence[2048] = {0};
    uint32_t frame = 0;
    uint64_t end = 0;
    int status = 1;
    (void)unused;
    for (;;) {
        uint32_t due;
        long result;
        if (sxn_wait_one(g_replay_stop,0) == 0) break;
        if (end && sxn_clock_ns() >= end) break;
        due = sxnaudio_frames_due(&g_record_output,50);
        if (due > 1024) due = 1024;
        if (frame < g_record_frames) {
            if (due > g_record_frames-frame) due = g_record_frames-frame;
            result = sxnaudio_write(&g_record_output,g_record_pcm+frame*2,due);
            if (result > 0) frame += (uint32_t)result;
        } else {
            // Drain with silence rather than extending the last input sample
            // through packet-loss concealment. The GUI never owns this stream.
            result = sxnaudio_write(&g_record_output,silence,due);
            if (!end) end = sxn_clock_ns()+200000000ull;
        }
        if (result < 0) { status = (int)result; break; }
        if (!due || !result) {
            result = sxn_wait_one(g_replay_stop,2);
            if (!result) break;
            if (result != -SAVANXP_ETIMEDOUT) { status = (int)result; break; }
        }
    }
    __atomic_store_n(&g_replay_result,status,__ATOMIC_RELEASE);
    sxn_thread_exit(status < 0 ? 1 : 0);
}
static void capture_error(long result) {
    const char* message = result == -SAVANXP_EBUSY ? "Input is in use by another application. Retrying..." :
        result == -SAVANXP_EACCES ? "Recording access denied." :
        result == -SAVANXP_EAGAIN ? "Input changed. Reconnecting..." :
        result == -SAVANXP_EIO ? "Input stopped responding. Retrying..." : "Recording device unavailable. Retrying...";
    (void)snprintf(g_record_text,sizeof(g_record_text),"%s",message);
}
static void recording_tick(uint64_t now) {
    int16_t samples[2048];
    unsigned int attempt;
    long result;
    if (g_capture.mode == SXNAUDIO_MODE_CLOSED && g_input_count && g_device >= 0 && now >= g_capture_retry_ms) {
        result = sxnaudio_capture_open(&g_capture);
        g_capture_last_ms = now; g_capture_retry_ms = now+1000;
        if (result < 0) capture_error(result);
    }
    for (attempt = 0; g_capture.mode != SXNAUDIO_MODE_CLOSED && attempt < 16; ++attempt) {
        result = sxnaudio_capture_read(&g_capture,samples,1024);
        if (result < 0 || (!result && now-g_capture_last_ms > 2000)) {
            sxnaudio_capture_close(&g_capture); g_capture_online = 0; g_capture_retry_ms = now+1000;
            if (g_record_state == RECORDING) { g_record_state = RECORD_IDLE; g_record_frames = 0; }
            capture_error(result < 0 ? result : -SAVANXP_EIO);
            break;
        }
        if (!result) break;
        if (!g_capture_online && g_record_state == RECORD_IDLE)
            (void)snprintf(g_record_text,sizeof(g_record_text),"Input level is live. Record a 3 second test.");
        g_capture_online = 1; g_capture_last_ms = now;
        if (g_capture.dropped_frames != g_capture_drops) {
            g_capture_drops = g_capture.dropped_frames;
            if (g_record_state == RECORDING) {
                g_record_state = RECORD_IDLE; g_record_frames = 0;
                (void)snprintf(g_record_text,sizeof(g_record_text),"Recording interrupted. Try again.");
            }
        }
        if (g_record_state == RECORDING) {
            uint32_t target = g_capture.rate_hz*3, frames = (uint32_t)result;
            if (target > 144000) target = 144000;
            if (frames > target-g_record_frames) frames = target-g_record_frames;
            memcpy(g_record_pcm+g_record_frames*2,samples,frames*4);
            g_record_frames += frames;
            (void)snprintf(g_record_text,sizeof(g_record_text),"Recording: %u.%u / 3.0 s",
                g_record_frames/g_capture.rate_hz,(g_record_frames*10/g_capture.rate_hz)%10);
            if (g_record_frames == target) {
                g_record_state = RECORD_READY;
                (void)snprintf(g_record_text,sizeof(g_record_text),"Recorded 3.0 s. Play the test to listen.");
            }
        }
    }
    if (g_record_state == RECORDING && now-g_record_started_ms > 5000) {
        g_record_state = RECORD_IDLE; g_record_frames = 0;
        (void)snprintf(g_record_text,sizeof(g_record_text),"Recording timed out. Try again.");
    }
    if (g_record_state == RECORD_PLAYING) {
        const int status = __atomic_load_n(&g_replay_result,__ATOMIC_ACQUIRE);
        if (status) {
            stop_record_output();
            (void)snprintf(g_record_text,sizeof(g_record_text),status < 0 ? "Playback stopped. Try again." :
                "Recorded 3.0 s. Play the test to listen.");
        }
    }
    g_app.tick_interval_ms = g_capture.mode != SXNAUDIO_MODE_CLOSED || g_record_state == RECORD_PLAYING ? 10 : 200;
}
static void on_record(struct sxgui_widget* widget, void* user) {
    (void)user;
    if (widget == &g_widgets[RECORD] && g_capture_online) {
        stop_record_output(); g_record_state = RECORDING; g_record_frames = 0;
        g_record_started_ms = sxn_clock_ns()/1000000;
        (void)snprintf(g_record_text,sizeof(g_record_text),"Recording: 0.0 / 3.0 s");
    } else if (widget == &g_widgets[PLAY_RECORDING] && g_record_state == RECORD_READY) {
        long result = sxnaudio_open(&g_record_output,"Sound recording test");
        if (result < 0 || g_record_output.rate_hz != g_capture.rate_hz) {
            if (result >= 0) sxnaudio_close(&g_record_output);
            (void)snprintf(g_record_text,sizeof(g_record_text),"Output unavailable or recording format changed.");
        } else {
            __atomic_store_n(&g_replay_result,0,__ATOMIC_RELAXED);
            g_replay_stop = sxn_event_create(SXN_EVENT_MANUAL_RESET);
            if (g_replay_stop >= 0)
                g_replay_thread = sxn_thread_create(replay_worker,0,g_replay_stack+sizeof(g_replay_stack),0);
            if (g_replay_stop < 0 || g_replay_thread < 0) {
                stop_record_output();
                (void)snprintf(g_record_text,sizeof(g_record_text),"Playback thread unavailable. Try again.");
            } else {
                g_record_state = RECORD_PLAYING; g_app.tick_interval_ms = 10;
                (void)snprintf(g_record_text,sizeof(g_record_text),"Playing the recorded test...");
            }
        }
    }
    refresh_outputs(); sxngui_app_request_repaint(&g_app);
}

static void disconnect(void) {
    sxnaudio_control_close(&g_control);
    memset(&g_snapshot,0,sizeof(g_snapshot));
    g_online = 0; g_pending = 0; g_selected = 0; g_route_pending = 0;
    g_policy_pending = 0; g_next_policy_ms = 0; g_policy_changing = 0;
    g_preferences_pending = 0; g_next_preferences_ms = 0;
    memset(&g_preferences,0,sizeof(g_preferences));
    memset(&g_policy,0,sizeof(g_policy));
    (void)snprintf(g_status,sizeof(g_status), "Audio service unavailable. Master controls remain available.");
    g_retry_ms = sxn_clock_ns() / 1000000 + 1000;
}
static void on_tick(struct sxngui_app* app) {
    uint64_t now = sxn_clock_ns() / 1000000;
    struct savanxp_audiod_snapshot snapshot;
    struct savanxp_audiod_endpoint_reply endpoint_reply;
    struct savanxp_audiod_jack_policy_reply policy_reply;
    struct savanxp_audiod_preference_reply preferences_reply;
    long result;
    recording_tick(now);
    if (now < g_next_ui_ms) return;
    g_next_ui_ms = now+200;
    if (g_device >= 0 && sxnaudio_master_get(g_device,&g_volume,&g_muted) < 0) {
        (void)sxn_close(g_device); g_device = -1;
    } else if (g_device >= 0 && !g_initial_master_known) {
        g_initial_volume = g_volume; g_initial_muted = g_muted; g_initial_master_known = 1;
    }
    if (g_device < 0 && now >= g_device_retry_ms) {
        g_device = sxn_open_device("audio0",SXN_DEVICE_READ | SXN_DEVICE_WRITE);
        g_device_retry_ms = now + 1000;
    }
    if (g_device < 0) {
        g_widgets[MASTER].flags |= SXGUI_FLAG_DISABLED;
        g_widgets[MASTER_MUTE].flags |= SXGUI_FLAG_DISABLED;
        (void)snprintf(g_status,sizeof(g_status), "No audio device available");
    } else {
        g_widgets[MASTER].flags &= ~SXGUI_FLAG_DISABLED;
        g_widgets[MASTER_MUTE].flags &= ~SXGUI_FLAG_DISABLED;

    }
    if (g_control.request < 0 && now >= g_retry_ms) {
        if (sxnaudio_control_open(&g_control) < 0) disconnect();
    }
    if (g_control.request >= 0) {
        while ((result = sxnaudio_control_receive_full(&g_control,&snapshot,&endpoint_reply,&policy_reply,&preferences_reply)) > 0) {
            if (result == 4) {
                if (g_preferences_pending == preferences_reply.request_id) {
                    g_preferences = preferences_reply; g_preferences_pending = 0;
                }
                continue;
            }
            if (result == 3) {
                if (g_policy_pending && policy_reply.request_id == g_policy_pending) {
                    g_policy = policy_reply; g_policy_pending = 0; g_policy_changing = 0;
                    if (policy_reply.result < 0) {
                        (void)snprintf(g_route_error,sizeof(g_route_error),"Automatic switching could not be changed (%d).",policy_reply.result);
                        g_route_error_until = now + 10000;
                    }
                }
                continue;
            }
            if (result == 2) {
                if (g_route_pending && endpoint_reply.request_id == g_route_pending && endpoint_reply.direction == g_route_direction) {
                    g_route_pending = 0;
                    g_next_policy_ms = 0;
                    if (endpoint_reply.result < 0) {
                        (void)snprintf(g_route_error,sizeof(g_route_error),"%s change failed (%d). %s",g_route_direction == SAVANXP_AUDIO_CAPTURE ? "Input" : "Output",
                            endpoint_reply.result, endpoint_reply.endpoint_id ? "Previous connector retained." : "Select a connector to retry.");
                        g_route_error_until = now + 10000;
                    } else {
                        g_route_error_until = 0;
                        if (g_route_direction == SAVANXP_AUDIO_PLAYBACK) {
                            g_policy.enabled = 0; g_policy.last_error = 0;
                        } else if (endpoint_reply.endpoint_id != g_route_previous_id) {
                            stop_record_output(); sxnaudio_capture_close(&g_capture);
                            g_capture_online = 0; g_capture_retry_ms = 0; g_capture_drops = 0;
                            g_record_state = RECORD_IDLE; g_record_frames = 0;
                            (void)snprintf(g_record_text,sizeof(g_record_text),"Input changed. Record a new test.");
                        }
                    }
                }
                continue;
            }
            if (g_pending && snapshot.request_id == g_pending) {
                g_snapshot = snapshot; g_pending = 0; g_online = 1;
                (void)snprintf(g_status,sizeof(g_status), "%u application%s%s",snapshot.count,
                    snapshot.count == 1 ? "" : "s", snapshot.count ? " - select one to adjust" : " playing");
            }
        }
        if (result < 0 || (g_pending && now - g_query_ms > 1000) ||
            (g_route_pending && now - g_route_ms > 1000) ||
            (g_policy_pending && now - g_policy_ms > 1000)) disconnect();
        else if (!g_pending) {
            ++g_sequence; if (!g_sequence) ++g_sequence;
            result = sxnaudio_control_query(&g_control,g_sequence);
            if (result == 0) { g_pending = g_sequence; g_query_ms = now; }
            else if (result != -SAVANXP_EAGAIN) disconnect();
        }
    }
    if (g_control.request >= 0 && !g_policy_pending && now >= g_next_policy_ms) {
        ++g_sequence; if (!g_sequence) ++g_sequence;
        result = sxnaudio_jack_policy_query(&g_control,g_sequence);
        if (result == 0) { g_policy_pending = g_sequence; g_policy_ms = now; g_next_policy_ms = now + 1000; }
        else if (result != -SAVANXP_EAGAIN) disconnect();
    }
    if (g_preferences_pending && now-g_preferences_ms > 2000) g_preferences_pending = 0;
    if (g_control.request >= 0 && !g_preferences_pending && now >= g_next_preferences_ms) {
        ++g_sequence; if (!g_sequence) ++g_sequence;
        result = sxnaudio_preferences_query(&g_control,g_sequence);
        if (!result) { g_preferences_pending = g_sequence; g_preferences_ms = now; g_next_preferences_ms = now+1000; }
        else if (result != -SAVANXP_EAGAIN) disconnect();
    }
    if (g_preferences.last_error < 0)
        (void)snprintf(g_status,sizeof(g_status),g_preferences.flags&SAVANXP_AUDIOD_PREFERENCE_LOAD_FAILED ?
            "Saved sound settings were invalid; using available defaults." : "Sound settings could not be saved.");
    if (g_policy.last_error < 0)
        (void)snprintf(g_status,sizeof(g_status),"Automatic output change failed (%d). Select an output to retry.",g_policy.last_error);
    if (g_route_pending) (void)snprintf(g_status,sizeof(g_status),"Changing %s...",
        g_route_direction == SAVANXP_AUDIO_CAPTURE ? "input" : "output");
    else if (g_policy_changing) (void)snprintf(g_status,sizeof(g_status),"Changing automatic switching...");
    else if (now < g_route_error_until) (void)snprintf(g_status,sizeof(g_status),"%s",g_route_error);
    refresh_outputs();
    refresh_controls();
    sxngui_app_request_repaint(app);
}
static void on_control(struct sxgui_widget* widget, void* user) {
    struct savanxp_audiod_stream_state* stream = selected();
    long result = 0;
    (void)user;
    if (widget == &g_widgets[OUTPUT] || widget == &g_widgets[INPUT_SELECT]) {
        uint32_t count = widget == &g_widgets[OUTPUT] ? g_output_count : g_input_count;
        if (g_online && !g_route_pending && widget->value >= 0 && (uint32_t)widget->value < count) {
            uint64_t id = widget == &g_widgets[OUTPUT] ? g_outputs[widget->value].id : g_inputs[widget->value].id;
            g_route_direction = widget == &g_widgets[OUTPUT] ? SAVANXP_AUDIO_PLAYBACK : SAVANXP_AUDIO_CAPTURE;
            g_route_previous_id = 0;
            if (g_route_direction == SAVANXP_AUDIO_CAPTURE) {
                uint32_t i;
                for (i = 0; i < g_input_count; ++i)
                    if (g_inputs[i].flags & SAVANXP_AUDIO_SELECTED) g_route_previous_id = g_inputs[i].id;
            }
            ++g_sequence; if (!g_sequence) ++g_sequence;
            result = sxnaudio_endpoint_select(&g_control,g_sequence,id,g_route_direction);
            if (result == 0) {
                g_route_pending = g_sequence; g_route_ms = sxn_clock_ns()/1000000;
                /* Discard a policy reply that could predate this manual choice. */
                g_policy_pending = 0; g_policy_changing = 0;
            }
            refresh_outputs();
        }
    } else if (widget == &g_widgets[JACK_POLICY]) {
        ++g_sequence; if (!g_sequence) ++g_sequence;
        result = sxnaudio_jack_policy_set(&g_control,g_sequence,(uint32_t)widget->value);
        if (result == 0) { g_policy_pending = g_sequence; g_policy_ms = sxn_clock_ns()/1000000; g_policy_changing = 1; }
        refresh_outputs();
    } else if (widget == &g_widgets[INPUT_VOLUME]) {
        result = sxnaudio_capture_volume(g_device,(uint32_t)widget->value);
    } else if (widget == &g_widgets[INPUT_MUTE]) {
        result = sxnaudio_capture_muted(g_device,(uint32_t)widget->value);
    } else if (widget == &g_widgets[MASTER]) {
        result = sxnaudio_master_volume(g_device,(uint32_t)widget->value);
        if (result >= 0) g_volume = (uint32_t)widget->value;
    } else if (widget == &g_widgets[MASTER_MUTE]) {
        result = sxnaudio_master_muted(g_device,(uint32_t)widget->value);
        if (result >= 0) g_muted = (uint32_t)widget->value;
    } else if (widget == &g_widgets[LIST]) {
        if (widget->value >= 0 && (uint32_t)widget->value < g_snapshot.count)
            g_selected = g_snapshot.streams[widget->value].id;
    } else if (widget == &g_widgets[HW_LIST]) {
        if (widget->value >= 0 && (uint32_t)widget->value < g_hw_count) g_hw_selected_id = g_hw[widget->value].id;
        refresh_outputs();
    } else if (stream) {
        uint16_t kind = widget == &g_widgets[APP_MUTE] ? SAVANXP_AUDIOD_STREAM_MUTED : SAVANXP_AUDIOD_STREAM_VOLUME;
        result = sxnaudio_control_change(&g_control,stream,kind,(uint8_t)widget->value);
        if (result == 0) {
            /* A reply to an earlier query may predate this change. Discard it
             * and request a fresh snapshot after the command in pipe order. */
            g_pending = 0;
            if (kind == SAVANXP_AUDIOD_STREAM_MUTED) stream->muted = (uint8_t)widget->value;
            else stream->volume = (uint8_t)widget->value;
        }
    }
    if (result < 0) (void)snprintf(g_status,sizeof(g_status), "The audio change could not be applied. Try again.");
    refresh_controls(); sxngui_app_request_repaint(&g_app);
}
/* Changes apply as they are made, as the volume slider always did, so OK only
 * closes. Cancel puts the device levels back as they were when the sheet
 * opened; connector choices and application levels stay applied. */
static void on_close(struct sxgui_widget* widget, void* user) {
    (void)user;
    if (widget == &g_widgets[CANCEL] && g_device >= 0) {
        if (g_initial_master_known) {
            (void)sxnaudio_master_volume(g_device,g_initial_volume);
            (void)sxnaudio_master_muted(g_device,g_initial_muted);
        }
        if (g_initial_input_known) {
            (void)sxnaudio_capture_volume(g_device,g_initial_input_volume);
            (void)sxnaudio_capture_muted(g_device,g_initial_input_muted);
        }
    }
    sxngui_app_quit(&g_app,0);
}
static void flush_preferences_on_exit(void) {
    struct savanxp_audiod_snapshot snapshot;
    struct savanxp_audiod_endpoint_reply endpoint;
    struct savanxp_audiod_jack_policy_reply policy;
    struct savanxp_audiod_preference_reply preferences;
    const uint64_t deadline = sxn_clock_ns()+2000000000ull;
    long pause;
    uint32_t id;
    int sent = 0;
    if (g_control.request < 0) return;
    id = ++g_sequence; if (!id) id = ++g_sequence;
    pause = sxn_event_create(0);
    while (sxn_clock_ns() < deadline) {
        long result;
        if (!sent) {
            result = sxnaudio_preferences_flush(&g_control,id);
            if (!result) sent = 1;
            else if (result != -SAVANXP_EAGAIN) break;
        }
        while ((result = sxnaudio_control_receive_full(&g_control,&snapshot,&endpoint,&policy,&preferences)) > 0) {
            if (result != 4 || preferences.request_id != id) continue;
            if (preferences.result < 0 && preferences.result != -SAVANXP_ENOSYS)
                (void)sxn_log("sound: preferences could not be saved before exit");
            (void)sxn_close(pause); return;
        }
        if (result < 0) break;
        (void)sxn_wait_one(pause,10);
    }
    (void)sxn_log("sound: timed out saving preferences; background saving remains active");
    (void)sxn_close(pause);
}
/* ---- the property sheet ---------------------------------------------------- */

static const signed char g_page_of[WIDGETS] = {
    [TABS] = PAGE_ALL,
    [DEVICE_NAME] = PAGE_VOLUME, [VOLUME_GROUP] = PAGE_VOLUME, [MASTER] = PAGE_VOLUME,
    [MASTER_TEXT] = PAGE_VOLUME, [MASTER_LEVEL] = PAGE_VOLUME, [MASTER_MUTE] = PAGE_VOLUME,
    [SPEAKER_GROUP] = PAGE_VOLUME, [JACK_POLICY] = PAGE_VOLUME,
    [LIST] = PAGE_APPLICATIONS, [APP_GROUP] = PAGE_APPLICATIONS, [SELECTED] = PAGE_APPLICATIONS,
    [APP_VOLUME] = PAGE_APPLICATIONS, [APP_TEXT] = PAGE_APPLICATIONS, [APP_LEVEL] = PAGE_APPLICATIONS,
    [APP_MUTE] = PAGE_APPLICATIONS,
    [PLAYBACK_GROUP] = PAGE_AUDIO, [OUTPUT] = PAGE_AUDIO, [RECORDING_GROUP] = PAGE_AUDIO,
    [INPUT_SELECT] = PAGE_AUDIO, [INPUT_VOLUME] = PAGE_AUDIO, [INPUT_TEXT] = PAGE_AUDIO,
    [INPUT_LEVEL] = PAGE_AUDIO, [INPUT_MUTE] = PAGE_AUDIO,
    [TEST_GROUP] = PAGE_VOICE, [VOICE_LEVEL] = PAGE_VOICE, [RECORD] = PAGE_VOICE,
    [PLAY_RECORDING] = PAGE_VOICE, [RECORD_STATUS] = PAGE_VOICE,
    [HW_LIST] = PAGE_HARDWARE, [HW_GROUP] = PAGE_HARDWARE, [HW_DEVICE] = PAGE_HARDWARE,
    [HW_CONNECTOR] = PAGE_HARDWARE, [HW_STATUS] = PAGE_HARDWARE,
    [STATUS] = PAGE_ALL, [OK] = PAGE_ALL, [CANCEL] = PAGE_ALL, [APPLY] = PAGE_ALL,
};
/* sxgui has no widget hierarchy: a page's controls exist only while their
 * tab is active, or they would paint and take clicks on the other pages. */
static void show_page(void) {
    int index;
    for (index = 0; index < WIDGETS; ++index) {
        if (g_page_of[index] == PAGE_ALL || g_page_of[index] == g_widgets[TABS].value)
            g_widgets[index].flags |= SXGUI_FLAG_VISIBLE;
        else g_widgets[index].flags &= ~(uint32_t)SXGUI_FLAG_VISIBLE;
    }
}
static void on_tab(struct sxgui_widget* widget, void* user) {
    (void)widget; (void)user;
    show_page(); sxngui_app_request_repaint(&g_app);
}

/* Hand-drawn 32x32 glyphs in the spirit of the classic sheet's artwork. */
static void paint_speaker(struct sx_painter* painter, int x, int y) {
    const struct sx_point cone[] = {{x+9,y+11},{x+18,y+4},{x+18,y+28},{x+9,y+21}};
    sx_painter_fill_rect(painter,sx_rect_make(x+1,y+11,9,10),SXGUI_COLOR_FACE);
    sxchrome_draw_raised(painter,sx_rect_make(x+1,y+11,9,10));
    sx_painter_fill_polygon(painter,cone,4,SXGUI_COLOR_BEVEL);
    sx_painter_draw_polyline(painter,cone,4,1,SXGUI_COLOR_DARK);
    sx_painter_draw_line(painter,x+10,y+12,x+17,y+7,SXGUI_COLOR_LIGHT);
    if (sx_painter_push_clip(painter,sx_rect_make(x+21,y+1,12,30))) {
        sx_painter_draw_ellipse(painter,sx_rect_make(x+11,y+8,14,16),SXGUI_RGB(0,0,128));
        sx_painter_draw_ellipse(painter,sx_rect_make(x+7,y+2,24,28),SXGUI_RGB(0,0,128));
        sx_painter_pop_clip(painter);
    }
}
static void paint_microphone(struct sx_painter* painter, int x, int y) {
    int step;
    for (step = 0; step < 3; ++step)
        sx_painter_draw_line(painter,x+5+step,y+30,x+16+step,y+13,SXGUI_COLOR_DARK);
    sx_painter_draw_line(painter,x+4,y+31,x+10,y+31,SXGUI_COLOR_DARK);
    sx_painter_fill_ellipse(painter,sx_rect_make(x+14,y+2,15,15),SXGUI_RGB(0,160,160));
    sx_painter_draw_ellipse(painter,sx_rect_make(x+14,y+2,15,15),SXGUI_RGB(0,64,64));
    sx_painter_draw_line(painter,x+17,y+12,x+25,y+5,SXGUI_RGB(0,96,96));
    sx_painter_draw_line(painter,x+19,y+14,x+27,y+7,SXGUI_RGB(0,96,96));
    sx_painter_fill_rect(painter,sx_rect_make(x+18,y+5,3,3),SXGUI_RGB(192,255,255));
}
static void paint_speaker_pair(struct sx_painter* painter, int x, int y) {
    const uint32_t dark = SXGUI_RGB(80,80,104), light = SXGUI_RGB(224,224,232);
    sx_painter_fill_round_rect(painter,sx_rect_make(x,y,22,58),3,dark);
    sx_painter_draw_round_rect(painter,sx_rect_make(x,y,22,58),3,SXGUI_COLOR_DARK);
    sx_painter_vline(painter,x+2,y+3,52,SXGUI_RGB(144,144,176));
    sx_painter_fill_round_rect(painter,sx_rect_make(x+26,y+4,18,54),3,light);
    sx_painter_draw_round_rect(painter,sx_rect_make(x+26,y+4,18,54),3,SXGUI_COLOR_SHADOW);
    sx_painter_fill_ellipse(painter,sx_rect_make(x+31,y+40,8,8),SXGUI_COLOR_SHADOW);
    sx_painter_fill_ellipse(painter,sx_rect_make(x+32,y+12,6,6),SXGUI_COLOR_SHADOW);
    sx_painter_fill_rect(painter,sx_rect_make(x+5,y+50,4,3),SXGUI_RGB(0,200,0));
}
/* A caption that greys out with the control it describes. */
static void paint_caption(struct sx_painter* painter, int x, int y, const char* text, int widget) {
    if (g_widgets[widget].flags & SXGUI_FLAG_DISABLED) sxchrome_draw_text_disabled(painter,x,y,text);
    else sx_painter_draw_text(painter,x,y,text,SXGUI_COLOR_TEXT);
}
static void paint_lines(struct sx_painter* painter, int x, int y, const char* const* lines, int count) {
    const int step = sx_painter_text_height(painter)+3;
    int index;
    for (index = 0; index < count; ++index)
        sx_painter_draw_text(painter,x,y+index*step,lines[index],SXGUI_COLOR_TEXT);
}
static void paint_panel(struct sxngui_app* app) {
    struct sx_painter* painter = &app->ui.painter;
    const struct sx_rect page = sxgui_tabs_page(&g_widgets[TABS]);
    const int left = page.x+10;
    switch (g_widgets[TABS].value) {
    case PAGE_VOLUME: {
        static const char* const speaker_text[] = {
            "Use these settings to let the speakers hand", "over to headphones when they are plugged in."};
        const struct sx_rect slider = g_widgets[MASTER].rect;
        paint_speaker(painter,left+4,page.y+10);
        paint_speaker(painter,g_widgets[VOLUME_GROUP].rect.x+12,slider.y-2);
        paint_caption(painter,slider.x+4,slider.y+slider.height+1,"Low",MASTER);
        paint_caption(painter,slider.x+slider.width-4-sx_painter_text_width(painter,"High"),
                      slider.y+slider.height+1,"High",MASTER);
        paint_speaker_pair(painter,g_widgets[SPEAKER_GROUP].rect.x+12,g_widgets[SPEAKER_GROUP].rect.y+22);
        paint_lines(painter,g_widgets[JACK_POLICY].rect.x,g_widgets[SPEAKER_GROUP].rect.y+24,speaker_text,2);
        break;
    }
    case PAGE_APPLICATIONS:
        sx_painter_draw_text(painter,left,page.y+12,"Applications playing sound:",SXGUI_COLOR_TEXT);
        break;
    case PAGE_AUDIO:
        paint_speaker(painter,g_widgets[PLAYBACK_GROUP].rect.x+12,g_widgets[PLAYBACK_GROUP].rect.y+24);
        paint_caption(painter,g_widgets[OUTPUT].rect.x,g_widgets[OUTPUT].rect.y-18,"Default device:",OUTPUT);
        paint_microphone(painter,g_widgets[RECORDING_GROUP].rect.x+12,g_widgets[RECORDING_GROUP].rect.y+24);
        paint_caption(painter,g_widgets[INPUT_SELECT].rect.x,g_widgets[INPUT_SELECT].rect.y-18,"Default device:",INPUT_SELECT);
        break;
    case PAGE_VOICE: {
        static const char* const voice_text[] = {
            "These settings test the default recording device:",
            "record a short sample, then play it back through",
            "the default playback device."};
        paint_lines(painter,left,page.y+12,voice_text,3);
        paint_microphone(painter,g_widgets[TEST_GROUP].rect.x+12,g_widgets[TEST_GROUP].rect.y+24);
        paint_caption(painter,g_widgets[VOICE_LEVEL].rect.x,g_widgets[VOICE_LEVEL].rect.y-18,"Input level:",RECORD);
        break;
    }
    case PAGE_HARDWARE:
        sx_painter_draw_text(painter,left,page.y+12,"Devices:",SXGUI_COLOR_TEXT);
        break;
    default:
        break;
    }
}
static void layout(struct sxngui_app* app) {
    const int width = (int)app->info.width, height = (int)app->info.height;
    struct sx_rect page;
    int x, y, w, gx, gw;
    g_widgets[TABS].rect = sx_rect_make(8,8,width-16,height-52);
    page = sxgui_tabs_page(&g_widgets[TABS]);
    x = page.x+10; y = page.y; w = page.width-20;
    gx = x+56; gw = w-68;

    /* Volume */
    g_widgets[DEVICE_NAME].rect = sx_rect_make(x+48,y+16,w-48,20);
    g_widgets[VOLUME_GROUP].rect = sx_rect_make(x,y+56,w,134);
    g_widgets[MASTER].rect = sx_rect_make(gx,y+80,gw-52,SXGUI_TRACKBAR_HEIGHT);
    g_widgets[MASTER_TEXT].rect = sx_rect_make(x+w-56,y+82,44,22);
    g_widgets[MASTER_LEVEL].rect = sx_rect_make(gx+4,y+128,gw-60,8);
    g_widgets[MASTER_MUTE].rect = sx_rect_make(gx,y+154,160,22);
    g_widgets[SPEAKER_GROUP].rect = sx_rect_make(x,y+204,w,112);
    g_widgets[JACK_POLICY].rect = sx_rect_make(gx+12,y+270,gw-12,22);

    /* Applications */
    g_widgets[LIST].rect = sx_rect_make(x,y+32,w,150);
    g_widgets[APP_GROUP].rect = sx_rect_make(x,y+194,w,124);
    g_widgets[SELECTED].rect = sx_rect_make(x+12,y+214,w-24,20);
    g_widgets[APP_VOLUME].rect = sx_rect_make(x+12,y+236,w-80,SXGUI_TRACKBAR_HEIGHT);
    g_widgets[APP_TEXT].rect = sx_rect_make(x+w-56,y+238,44,22);
    g_widgets[APP_LEVEL].rect = sx_rect_make(x+16,y+268,w-88,8);
    g_widgets[APP_MUTE].rect = sx_rect_make(x+12,y+288,120,22);

    /* Audio */
    g_widgets[PLAYBACK_GROUP].rect = sx_rect_make(x,y+10,w,86);
    g_widgets[OUTPUT].rect = sx_rect_make(gx,y+52,gw,24);
    g_widgets[RECORDING_GROUP].rect = sx_rect_make(x,y+106,w,164);
    g_widgets[INPUT_SELECT].rect = sx_rect_make(gx,y+148,gw,24);
    g_widgets[INPUT_VOLUME].rect = sx_rect_make(gx,y+180,gw-52,SXGUI_TRACKBAR_HEIGHT);
    g_widgets[INPUT_TEXT].rect = sx_rect_make(x+w-56,y+182,44,22);
    g_widgets[INPUT_LEVEL].rect = sx_rect_make(gx+4,y+214,gw-60,8);
    g_widgets[INPUT_MUTE].rect = sx_rect_make(gx,y+236,120,22);

    /* Voice */
    g_widgets[TEST_GROUP].rect = sx_rect_make(x,y+72,w,150);
    g_widgets[VOICE_LEVEL].rect = sx_rect_make(gx,y+114,gw,8);
    g_widgets[RECORD].rect = sx_rect_make(gx,y+140,120,26);
    g_widgets[PLAY_RECORDING].rect = sx_rect_make(gx+128,y+140,130,26);
    g_widgets[RECORD_STATUS].rect = sx_rect_make(gx,y+178,gw,22);

    /* Hardware */
    g_widgets[HW_LIST].rect = sx_rect_make(x,y+32,w,150);
    g_widgets[HW_GROUP].rect = sx_rect_make(x,y+194,w,100);
    g_widgets[HW_DEVICE].rect = sx_rect_make(x+12,y+216,w-24,20);
    g_widgets[HW_CONNECTOR].rect = sx_rect_make(x+12,y+240,w-24,20);
    g_widgets[HW_STATUS].rect = sx_rect_make(x+12,y+264,w-24,20);

    g_widgets[STATUS].rect = sx_rect_make(x,page.y+page.height-32,w,22);
    g_widgets[OK].rect = sx_rect_make(width-280,height-34,80,26);
    g_widgets[CANCEL].rect = sx_rect_make(width-192,height-34,80,26);
    g_widgets[APPLY].rect = sx_rect_make(width-104,height-34,80,26);
    sxngui_app_request_repaint(app);
}
int main(int argc, char** argv) {
    int result, width;
    (void)argc; (void)argv;
    g_widgets[TABS] = sxgui_tabs(sx_rect_make(0,0,0,0),g_tab_labels,TAB_COUNT,PAGE_VOLUME);
    g_widgets[TABS].on_action = on_tab;

    g_widgets[DEVICE_NAME] = sxgui_label(sx_rect_make(0,0,0,0),g_device_text);
    g_widgets[VOLUME_GROUP] = sxgui_groupbox(sx_rect_make(0,0,0,0),"Device volume");
    g_widgets[MASTER] = sxgui_trackbar(sx_rect_make(0,0,0,0),0,100,100);
    g_widgets[MASTER_TEXT] = sxgui_label(sx_rect_make(0,0,0,0),g_master_text);
    g_widgets[MASTER_LEVEL] = sxgui_progress(sx_rect_make(0,0,0,0),0,100,0);
    g_widgets[MASTER_MUTE] = sxgui_checkbox(sx_rect_make(0,0,0,0),"Mute",0);
    g_widgets[SPEAKER_GROUP] = sxgui_groupbox(sx_rect_make(0,0,0,0),"Speaker settings");
    g_widgets[JACK_POLICY] = sxgui_checkbox(sx_rect_make(0,0,0,0),"Automatically switch to headphones",0);

    g_widgets[LIST] = sxgui_listbox(sx_rect_make(0,0,0,0),g_row_ptrs,0);
    g_widgets[LIST].columns = g_columns; g_widgets[LIST].column_count = 4;
    g_widgets[APP_GROUP] = sxgui_groupbox(sx_rect_make(0,0,0,0),"Application volume");
    g_widgets[SELECTED] = sxgui_label(sx_rect_make(0,0,0,0),g_selected_text);
    g_widgets[APP_VOLUME] = sxgui_trackbar(sx_rect_make(0,0,0,0),0,100,100);
    g_widgets[APP_TEXT] = sxgui_label(sx_rect_make(0,0,0,0),g_app_text);
    g_widgets[APP_LEVEL] = sxgui_progress(sx_rect_make(0,0,0,0),0,100,0);
    g_widgets[APP_MUTE] = sxgui_checkbox(sx_rect_make(0,0,0,0),"Mute",0);

    g_widgets[PLAYBACK_GROUP] = sxgui_groupbox(sx_rect_make(0,0,0,0),"Sound playback");
    g_widgets[OUTPUT] = sxgui_combobox(sx_rect_make(0,0,0,0),g_output_ptrs,0,-1);
    g_widgets[RECORDING_GROUP] = sxgui_groupbox(sx_rect_make(0,0,0,0),"Sound recording");
    g_widgets[INPUT_SELECT] = sxgui_combobox(sx_rect_make(0,0,0,0),g_input_ptrs,0,-1);
    g_widgets[INPUT_VOLUME] = sxgui_trackbar(sx_rect_make(0,0,0,0),0,100,100);
    g_widgets[INPUT_TEXT] = sxgui_label(sx_rect_make(0,0,0,0),g_input_text);
    g_widgets[INPUT_LEVEL] = sxgui_progress(sx_rect_make(0,0,0,0),0,100,0);
    g_widgets[INPUT_MUTE] = sxgui_checkbox(sx_rect_make(0,0,0,0),"Mute",0);

    g_widgets[TEST_GROUP] = sxgui_groupbox(sx_rect_make(0,0,0,0),"Hardware test");
    g_widgets[VOICE_LEVEL] = sxgui_progress(sx_rect_make(0,0,0,0),0,100,0);
    g_widgets[RECORD] = sxgui_button(sx_rect_make(0,0,0,0),"Record test",on_record,0);
    g_widgets[PLAY_RECORDING] = sxgui_button(sx_rect_make(0,0,0,0),"Play recording",on_record,0);
    g_widgets[RECORD_STATUS] = sxgui_label(sx_rect_make(0,0,0,0),g_record_text);

    g_widgets[HW_LIST] = sxgui_listbox(sx_rect_make(0,0,0,0),g_hw_ptrs,0);
    g_widgets[HW_LIST].columns = g_hw_columns; g_widgets[HW_LIST].column_count = 2;
    g_widgets[HW_GROUP] = sxgui_groupbox(sx_rect_make(0,0,0,0),"Device properties");
    g_widgets[HW_DEVICE] = sxgui_label(sx_rect_make(0,0,0,0),g_hw_device);
    g_widgets[HW_CONNECTOR] = sxgui_label(sx_rect_make(0,0,0,0),g_hw_connector);
    g_widgets[HW_STATUS] = sxgui_label(sx_rect_make(0,0,0,0),g_hw_status);

    g_widgets[STATUS] = sxgui_label(sx_rect_make(0,0,0,0),g_status);
    g_widgets[OK] = sxgui_button(sx_rect_make(0,0,0,0),"OK",on_close,0);
    g_widgets[CANCEL] = sxgui_button(sx_rect_make(0,0,0,0),"Cancel",on_close,0);
    /* Nothing waits to be applied: every control takes effect at once. */
    g_widgets[APPLY] = sxgui_button(sx_rect_make(0,0,0,0),"Apply",on_close,0);
    g_widgets[APPLY].flags |= SXGUI_FLAG_DISABLED;

    g_widgets[OUTPUT].on_action = g_widgets[JACK_POLICY].on_action = on_control;
    g_widgets[MASTER].on_action = g_widgets[MASTER_MUTE].on_action = on_control;
    g_widgets[LIST].on_action = g_widgets[APP_VOLUME].on_action = g_widgets[APP_MUTE].on_action = on_control;
    g_widgets[INPUT_SELECT].on_action = g_widgets[INPUT_VOLUME].on_action = g_widgets[INPUT_MUTE].on_action = on_control;
    g_widgets[HW_LIST].on_action = on_control;
    (void)snprintf(g_status,sizeof(g_status), "Connecting to audio service...");
    show_page();
    result = sxngui_app_init(&g_app,&sun_resources,g_widgets,WIDGETS);
    if (result < 0) { (void)sxn_log("sound: window unavailable"); sxn_exit(1); }
    g_app.on_resize = layout; g_app.on_paint = paint_panel;
    g_app.on_tick = on_tick; g_app.tick_interval_ms = 200;
    width = sxgui_tabs_preferred_width(g_tab_labels,TAB_COUNT)+40;
    (void)sxngui_app_set_content_size(&g_app,width > 440 ? width : 440,450);
    layout(&g_app); on_tick(&g_app);
    result = sxngui_app_run(&g_app);
    stop_record_output(); sxnaudio_capture_close(&g_capture);
    flush_preferences_on_exit();
    sxnaudio_control_close(&g_control);
    if (g_device >= 0) (void)sxn_close(g_device);
    sxn_exit(result);
    return 0;
}
