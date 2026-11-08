/* Sound: the SUN/SxGUI control panel for audiod. Live controls are separate
 * from playback streams; a panel never claims the device's PCM writer. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "sxn/sxngui.h"
#include "sxn/sxnaudio.h"
#include "sxn/sxncapture.h"
#include "sound.sunres.h"

enum { OUTPUT, MASTER, MASTER_MUTE, MASTER_LEVEL, MASTER_TEXT, JACK_POLICY, APPLICATIONS,
       LIST, SELECTED, APP_VOLUME, APP_MUTE, APP_LEVEL, APP_TEXT,
       INPUT_TITLE, INPUT_SELECT, INPUT_VOLUME, INPUT_MUTE, INPUT_LEVEL, INPUT_TEXT,
       RECORD, PLAY_RECORDING, RECORD_STATUS, STATUS, CLOSE, WIDGETS };
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
static uint32_t g_record_frames, g_play_frame;
static uint64_t g_capture_retry_ms, g_capture_last_ms, g_record_started_ms, g_play_end_ms, g_capture_drops, g_next_ui_ms;
static char g_input_text[32], g_record_text[100];

static uint64_t g_query_ms, g_retry_ms, g_device_retry_ms;
static int g_online;
static char g_master_text[32], g_selected_text[64], g_app_text[32], g_status[100];
static char g_rows[SAVANXP_AUDIOD_MAX_STREAMS][100];
static const char* g_row_ptrs[SAVANXP_AUDIOD_MAX_STREAMS];
static const struct sxgui_column g_columns[] = {
    {"Application", 250, 0}, {"Volume", 70, 0}, {"Muted", 60, 0}, {"Level", 70, 0}
};

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
    g_output_count = g_input_count = 0;
    if (g_device >= 0) {
        for (index = 0; index < SAVANXP_AUDIO_MAX_ENDPOINTS; ++index) {
            if (sxnaudio_endpoint_get(g_device,index,&endpoint) < 0) break;
            const char* presence = !(endpoint.flags & SAVANXP_AUDIO_AVAILABLE) ? " (unavailable)" :
                !(endpoint.flags & SAVANXP_AUDIO_JACK) ? "" :
                !(endpoint.flags & SAVANXP_AUDIO_PRESENCE_CAPABLE) ? " (no detection)" :
                !(endpoint.flags & SAVANXP_AUDIO_PRESENCE_VALID) ? " (unknown)" :
                (endpoint.flags & SAVANXP_AUDIO_CONNECTED) ? " (connected)" : " (unplugged)";
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
    } else {
        g_widgets[INPUT_VOLUME].flags |= SXGUI_FLAG_DISABLED; g_widgets[INPUT_MUTE].flags |= SXGUI_FLAG_DISABLED;
        g_widgets[INPUT_LEVEL].value = 0; g_input_text[0] = 0;
        if (!g_input_count) (void)snprintf(g_record_text,sizeof(g_record_text),"No recording device available");
    }
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
    if (g_record_output.mode != SXNAUDIO_MODE_CLOSED) sxnaudio_close(&g_record_output);
    if (g_record_state == RECORD_PLAYING) g_record_state = g_record_frames ? RECORD_READY : RECORD_IDLE;
    g_play_end_ms = 0;
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
        unsigned int burst;
        for (burst = 0; burst < 8; ++burst) {
            uint32_t due = sxnaudio_frames_due(&g_record_output,50);
            if (!due) break;
            if (due > 480) due = 480;
            if (g_play_frame < g_record_frames) {
                if (due > g_record_frames-g_play_frame) due = g_record_frames-g_play_frame;
                result = sxnaudio_write(&g_record_output,g_record_pcm+g_play_frame*2,due);
                if (result >= 0) g_play_frame += (uint32_t)result;
            } else {
                // Silence drains the output without extending the final
                // captured sample through the daemon's concealment window.
                memset(samples,0,sizeof(samples));
                result = sxnaudio_write(&g_record_output,samples,due);
                if (!g_play_end_ms) g_play_end_ms = now+200;
            }
            if (result < 0) {
                stop_record_output();
                (void)snprintf(g_record_text,sizeof(g_record_text),"Playback stopped. Try again.");
                break;
            }
            if (!result) break;
        }
        if (g_play_end_ms && now >= g_play_end_ms) {
            stop_record_output();
            (void)snprintf(g_record_text,sizeof(g_record_text),"Recorded 3.0 s. Play the test to listen.");
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
            g_play_frame = 0; g_play_end_ms = 0; g_record_state = RECORD_PLAYING;
            g_app.tick_interval_ms = 10;
            (void)snprintf(g_record_text,sizeof(g_record_text),"Playing the recorded test...");
        }
    }
    refresh_outputs(); sxngui_app_request_repaint(&g_app);
}

static void disconnect(void) {
    sxnaudio_control_close(&g_control);
    memset(&g_snapshot,0,sizeof(g_snapshot));
    g_online = 0; g_pending = 0; g_selected = 0; g_route_pending = 0;
    g_policy_pending = 0; g_next_policy_ms = 0; g_policy_changing = 0;
    memset(&g_policy,0,sizeof(g_policy));
    (void)snprintf(g_status,sizeof(g_status), "Audio service unavailable. Master controls remain available.");
    g_retry_ms = sxn_clock_ns() / 1000000 + 1000;
}
static void on_tick(struct sxngui_app* app) {
    uint64_t now = sxn_clock_ns() / 1000000;
    struct savanxp_audiod_snapshot snapshot;
    struct savanxp_audiod_endpoint_reply endpoint_reply;
    struct savanxp_audiod_jack_policy_reply policy_reply;
    long result;
    recording_tick(now);
    if (now < g_next_ui_ms) return;
    g_next_ui_ms = now+200;
    if (g_device >= 0 && sxnaudio_master_get(g_device,&g_volume,&g_muted) < 0) {
        (void)sxn_close(g_device); g_device = -1;
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
        while ((result = sxnaudio_control_receive_ex(&g_control,&snapshot,&endpoint_reply,&policy_reply)) > 0) {
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
static void on_close(struct sxgui_widget* widget, void* user) {
    (void)widget; (void)user; sxngui_app_quit(&g_app,0);
}
static void layout(struct sxngui_app* app) {
    int width = (int)app->info.width, height = (int)app->info.height;
    int bottom = height - 304;
    if (bottom < 245) bottom = 245;
    g_widgets[OUTPUT].rect = sx_rect_make(12,12,width-24,22);
    g_widgets[MASTER].rect = sx_rect_make(12,40,width-172,30);
    g_widgets[MASTER_MUTE].rect = sx_rect_make(width-140,43,76,24);
    g_widgets[MASTER_TEXT].rect = sx_rect_make(width-60,43,48,24);
    g_widgets[MASTER_LEVEL].rect = sx_rect_make(12,80,width-24,14);
    g_widgets[JACK_POLICY].rect = sx_rect_make(12,100,width-24,22);
    g_widgets[APPLICATIONS].rect = sx_rect_make(12,132,width-24,22);
    g_widgets[LIST].rect = sx_rect_make(12,158,width-24,bottom-170);
    g_widgets[SELECTED].rect = sx_rect_make(12,bottom,width-24,20);
    g_widgets[APP_VOLUME].rect = sx_rect_make(12,bottom+24,width-172,30);
    g_widgets[APP_MUTE].rect = sx_rect_make(width-140,bottom+27,76,24);
    g_widgets[APP_TEXT].rect = sx_rect_make(width-60,bottom+27,48,24);
    g_widgets[APP_LEVEL].rect = sx_rect_make(12,bottom+64,width-24,14);
    g_widgets[INPUT_TITLE].rect = sx_rect_make(12,bottom+88,width-24,20);
    g_widgets[INPUT_SELECT].rect = sx_rect_make(12,bottom+112,width-24,24);
    g_widgets[INPUT_VOLUME].rect = sx_rect_make(12,bottom+144,width-172,30);
    g_widgets[INPUT_MUTE].rect = sx_rect_make(width-140,bottom+147,76,24);
    g_widgets[INPUT_TEXT].rect = sx_rect_make(width-60,bottom+147,48,24);
    g_widgets[INPUT_LEVEL].rect = sx_rect_make(12,bottom+184,width-24,14);
    g_widgets[RECORD].rect = sx_rect_make(12,bottom+208,120,28);
    g_widgets[PLAY_RECORDING].rect = sx_rect_make(144,bottom+208,140,28);
    g_widgets[RECORD_STATUS].rect = sx_rect_make(12,bottom+244,width-24,22);
    g_widgets[STATUS].rect = sx_rect_make(12,height-32,width-124,24);
    g_widgets[CLOSE].rect = sx_rect_make(width-96,height-34,84,26);
    sxngui_app_request_repaint(app);
}
int main(int argc, char** argv) {
    int result;
    (void)argc; (void)argv;
    g_widgets[OUTPUT] = sxgui_combobox(sx_rect_make(0,0,0,0),g_output_ptrs,0,-1);
    g_widgets[OUTPUT].on_action = on_control;
    g_widgets[MASTER] = sxgui_trackbar(sx_rect_make(0,0,0,0),0,100,100);
    g_widgets[MASTER_MUTE] = sxgui_checkbox(sx_rect_make(0,0,0,0),"Mute",0);
    g_widgets[MASTER_LEVEL] = sxgui_progress(sx_rect_make(0,0,0,0),0,100,0);
    g_widgets[MASTER_TEXT] = sxgui_label(sx_rect_make(0,0,0,0),g_master_text);
    g_widgets[JACK_POLICY] = sxgui_checkbox(sx_rect_make(0,0,0,0),"Automatically switch to headphones",0);
    g_widgets[JACK_POLICY].on_action = on_control;
    g_widgets[APPLICATIONS] = sxgui_label(sx_rect_make(0,0,0,0),"Applications");
    g_widgets[LIST] = sxgui_listbox(sx_rect_make(0,0,0,0),g_row_ptrs,0);
    g_widgets[LIST].columns = g_columns; g_widgets[LIST].column_count = 4;
    g_widgets[SELECTED] = sxgui_label(sx_rect_make(0,0,0,0),g_selected_text);
    g_widgets[APP_VOLUME] = sxgui_trackbar(sx_rect_make(0,0,0,0),0,100,100);
    g_widgets[APP_MUTE] = sxgui_checkbox(sx_rect_make(0,0,0,0),"Mute",0);
    g_widgets[APP_LEVEL] = sxgui_progress(sx_rect_make(0,0,0,0),0,100,0);
    g_widgets[APP_TEXT] = sxgui_label(sx_rect_make(0,0,0,0),g_app_text);
    g_widgets[INPUT_TITLE] = sxgui_label(sx_rect_make(0,0,0,0),"Recording");
    g_widgets[INPUT_SELECT] = sxgui_combobox(sx_rect_make(0,0,0,0),g_input_ptrs,0,-1);
    g_widgets[INPUT_VOLUME] = sxgui_trackbar(sx_rect_make(0,0,0,0),0,100,100);
    g_widgets[INPUT_MUTE] = sxgui_checkbox(sx_rect_make(0,0,0,0),"Mute",0);
    g_widgets[INPUT_LEVEL] = sxgui_progress(sx_rect_make(0,0,0,0),0,100,0);
    g_widgets[INPUT_TEXT] = sxgui_label(sx_rect_make(0,0,0,0),g_input_text);
    g_widgets[RECORD] = sxgui_button(sx_rect_make(0,0,0,0),"Record test",on_record,0);
    g_widgets[PLAY_RECORDING] = sxgui_button(sx_rect_make(0,0,0,0),"Play recording",on_record,0);
    g_widgets[RECORD_STATUS] = sxgui_label(sx_rect_make(0,0,0,0),g_record_text);
    g_widgets[INPUT_SELECT].on_action = g_widgets[INPUT_VOLUME].on_action = g_widgets[INPUT_MUTE].on_action = on_control;
    g_widgets[STATUS] = sxgui_label(sx_rect_make(0,0,0,0),g_status);
    g_widgets[CLOSE] = sxgui_button(sx_rect_make(0,0,0,0),"Close",on_close,0);
    g_widgets[MASTER].on_action = g_widgets[MASTER_MUTE].on_action = on_control;
    g_widgets[LIST].on_action = g_widgets[APP_VOLUME].on_action = g_widgets[APP_MUTE].on_action = on_control;
    (void)snprintf(g_status,sizeof(g_status), "Connecting to audio service...");
    result = sxngui_app_init(&g_app,&sun_resources,g_widgets,WIDGETS);
    if (result < 0) { (void)sxn_log("sound: window unavailable"); sxn_exit(1); }
    g_app.on_resize = layout; g_app.on_tick = on_tick; g_app.tick_interval_ms = 200;
    (void)sxngui_app_set_content_size(&g_app,600,640);
    layout(&g_app); on_tick(&g_app);
    result = sxngui_app_run(&g_app);
    stop_record_output(); sxnaudio_capture_close(&g_capture);
    sxnaudio_control_close(&g_control);
    if (g_device >= 0) (void)sxn_close(g_device);
    sxn_exit(result);
    return 0;
}
