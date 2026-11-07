/* Sound: the SUN/SxGUI control panel for audiod. Live controls are separate
 * from playback streams; a panel never claims the device's PCM writer. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "sxn/sxngui.h"
#include "sxn/sxnaudio.h"
#include "sound.sunres.h"

enum { OUTPUT, MASTER, MASTER_MUTE, MASTER_LEVEL, MASTER_TEXT, APPLICATIONS,
       LIST, SELECTED, APP_VOLUME, APP_MUTE, APP_LEVEL, APP_TEXT, STATUS, CLOSE, WIDGETS };
static struct sxngui_app g_app;
static struct sxgui_widget g_widgets[WIDGETS];
static struct sxnaudio_control g_control = {-1,-1};
static struct savanxp_audiod_snapshot g_snapshot;
static long g_device = -1;
static uint32_t g_volume = 100, g_muted, g_selected;
static uint32_t g_sequence, g_pending, g_route_pending;
static uint64_t g_route_ms, g_route_error_until;
static struct savanxp_audio_endpoint g_outputs[SAVANXP_AUDIO_MAX_ENDPOINTS];
static uint32_t g_output_count;
static char g_output_rows[SAVANXP_AUDIO_MAX_ENDPOINTS][100];
static const char* g_output_ptrs[SAVANXP_AUDIO_MAX_ENDPOINTS];
static char g_route_error[100];
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
    int active = -1;
    struct savanxp_audio_endpoint endpoint;
    g_output_count = 0;
    if (g_device >= 0) {
        for (index = 0; index < SAVANXP_AUDIO_MAX_ENDPOINTS; ++index) {
            if (sxnaudio_endpoint_get(g_device,index,&endpoint) < 0) break;
            if (!(endpoint.flags & SAVANXP_AUDIO_PLAYBACK)) continue;
            g_outputs[g_output_count] = endpoint;
            (void)snprintf(g_output_rows[g_output_count],sizeof(g_output_rows[0]),"%s - %s",
                           endpoint.device_name,endpoint.name);
            g_output_ptrs[g_output_count] = g_output_rows[g_output_count];
            if (endpoint.flags & SAVANXP_AUDIO_SELECTED) active = (int)g_output_count;
            ++g_output_count;
        }
    }
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
static void disconnect(void) {
    sxnaudio_control_close(&g_control);
    memset(&g_snapshot,0,sizeof(g_snapshot));
    g_online = 0; g_pending = 0; g_selected = 0; g_route_pending = 0;
    (void)snprintf(g_status,sizeof(g_status), "Audio service unavailable. Master controls remain available.");
    g_retry_ms = sxn_clock_ns() / 1000000 + 1000;
}
static void on_tick(struct sxngui_app* app) {
    uint64_t now = sxn_clock_ns() / 1000000;
    struct savanxp_audiod_snapshot snapshot;
    struct savanxp_audiod_endpoint_reply endpoint_reply;
    long result;
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
        while ((result = sxnaudio_control_receive(&g_control,&snapshot,&endpoint_reply)) > 0) {
            if (result == 2) {
                if (g_route_pending && endpoint_reply.request_id == g_route_pending) {
                    g_route_pending = 0;
                    if (endpoint_reply.result < 0) {
                        (void)snprintf(g_route_error,sizeof(g_route_error),"Output change failed (%d). %s",
                            endpoint_reply.result, endpoint_reply.endpoint_id ? "Previous output retained." : "Select an output to retry.");
                        g_route_error_until = now + 10000;
                    } else g_route_error_until = 0;
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
            (g_route_pending && now - g_route_ms > 1000)) disconnect();
        else if (!g_pending) {
            ++g_sequence; if (!g_sequence) ++g_sequence;
            result = sxnaudio_control_query(&g_control,g_sequence);
            if (result == 0) { g_pending = g_sequence; g_query_ms = now; }
            else if (result != -SAVANXP_EAGAIN) disconnect();
        }
    }
    if (g_route_pending) (void)snprintf(g_status,sizeof(g_status),"Changing output...");
    else if (now < g_route_error_until) (void)snprintf(g_status,sizeof(g_status),"%s",g_route_error);
    refresh_outputs();
    refresh_controls();
    sxngui_app_request_repaint(app);
}
static void on_control(struct sxgui_widget* widget, void* user) {
    struct savanxp_audiod_stream_state* stream = selected();
    long result = 0;
    (void)user;
    if (widget == &g_widgets[OUTPUT]) {
        if (g_online && !g_route_pending && widget->value >= 0 && (uint32_t)widget->value < g_output_count) {
            ++g_sequence; if (!g_sequence) ++g_sequence;
            result = sxnaudio_endpoint_select(&g_control,g_sequence,g_outputs[widget->value].id,SAVANXP_AUDIO_PLAYBACK);
            if (result == 0) { g_route_pending = g_sequence; g_route_ms = sxn_clock_ns()/1000000; }
            refresh_outputs();
        }
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
    int bottom = height - 124;
    if (bottom < 245) bottom = 245;
    g_widgets[OUTPUT].rect = sx_rect_make(12,12,width-24,22);
    g_widgets[MASTER].rect = sx_rect_make(12,40,width-172,30);
    g_widgets[MASTER_MUTE].rect = sx_rect_make(width-140,43,76,24);
    g_widgets[MASTER_TEXT].rect = sx_rect_make(width-60,43,48,24);
    g_widgets[MASTER_LEVEL].rect = sx_rect_make(12,80,width-24,14);
    g_widgets[APPLICATIONS].rect = sx_rect_make(12,106,width-24,22);
    g_widgets[LIST].rect = sx_rect_make(12,132,width-24,bottom-144);
    g_widgets[SELECTED].rect = sx_rect_make(12,bottom,width-24,20);
    g_widgets[APP_VOLUME].rect = sx_rect_make(12,bottom+24,width-172,30);
    g_widgets[APP_MUTE].rect = sx_rect_make(width-140,bottom+27,76,24);
    g_widgets[APP_TEXT].rect = sx_rect_make(width-60,bottom+27,48,24);
    g_widgets[APP_LEVEL].rect = sx_rect_make(12,bottom+64,width-24,14);
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
    g_widgets[APPLICATIONS] = sxgui_label(sx_rect_make(0,0,0,0),"Applications");
    g_widgets[LIST] = sxgui_listbox(sx_rect_make(0,0,0,0),g_row_ptrs,0);
    g_widgets[LIST].columns = g_columns; g_widgets[LIST].column_count = 4;
    g_widgets[SELECTED] = sxgui_label(sx_rect_make(0,0,0,0),g_selected_text);
    g_widgets[APP_VOLUME] = sxgui_trackbar(sx_rect_make(0,0,0,0),0,100,100);
    g_widgets[APP_MUTE] = sxgui_checkbox(sx_rect_make(0,0,0,0),"Mute",0);
    g_widgets[APP_LEVEL] = sxgui_progress(sx_rect_make(0,0,0,0),0,100,0);
    g_widgets[APP_TEXT] = sxgui_label(sx_rect_make(0,0,0,0),g_app_text);
    g_widgets[STATUS] = sxgui_label(sx_rect_make(0,0,0,0),g_status);
    g_widgets[CLOSE] = sxgui_button(sx_rect_make(0,0,0,0),"Close",on_close,0);
    g_widgets[MASTER].on_action = g_widgets[MASTER_MUTE].on_action = on_control;
    g_widgets[LIST].on_action = g_widgets[APP_VOLUME].on_action = g_widgets[APP_MUTE].on_action = on_control;
    (void)snprintf(g_status,sizeof(g_status), "Connecting to audio service...");
    result = sxngui_app_init(&g_app,&sun_resources,g_widgets,WIDGETS);
    if (result < 0) { (void)sxn_log("sound: window unavailable"); sxn_exit(1); }
    g_app.on_resize = layout; g_app.on_tick = on_tick; g_app.tick_interval_ms = 200;
    (void)sxngui_app_set_content_size(&g_app,600,460);
    layout(&g_app); on_tick(&g_app);
    result = sxngui_app_run(&g_app);
    sxnaudio_control_close(&g_control);
    if (g_device >= 0) (void)sxn_close(g_device);
    sxn_exit(result);
    return 0;
}
