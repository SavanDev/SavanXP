// Analog capture routing adapted from OpenBSD codec discovery and mixer verbs.
/*-
 * Copyright (c) 2005 The NetBSD Foundation, Inc.
 * All rights reserved.
 *
 * This code is derived from software contributed to The NetBSD Foundation
 * by TAMURA Kent
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE NETBSD FOUNDATION, INC. AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE FOUNDATION OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include "azalia_port.hpp"
namespace azalia_port {
namespace {
constexpr int kIoError = 5, kInvalid = 22;
bool input_pin(const codec_t* c, int nid) {
    if (!azalia_widget_enabled(c,nid)) return false;
    const auto& pin = c->w[nid];
    return pin.type == COP_AWTYPE_PIN_COMPLEX && !(pin.widgetcap & COP_AWCAP_DIGITAL) &&
        (pin.d.pin.cap & COP_PINCAP_INPUT) && !(pin.d.pin.cap & COP_PINCAP_HDMI) &&
        (pin.d.pin.device == CORB_CD_MICIN || pin.d.pin.device == CORB_CD_LINEIN || pin.d.pin.device == CORB_CD_AUX);
}
bool trace_input(codec_t* c, int node, int target, Path& path) {
    if (!azalia_widget_enabled(c,node) || path.count >= 10) return false;
    for (unsigned i = 0; i < path.count; ++i) if (path.nodes[i] == node) return false;
    const auto& w = c->w[node];
    if (w.widgetcap & COP_AWCAP_DIGITAL) return false;
    const unsigned here = path.count;
    path.nodes[path.count] = node; path.inputs[path.count++] = -1;
    if (node == target) return true;
    if (w.type == COP_AWTYPE_AUDIO_INPUT || w.type == COP_AWTYPE_AUDIO_MIXER || w.type == COP_AWTYPE_AUDIO_SELECTOR) {
        for (int i = 0; i < w.nconnections; ++i) {
            path.inputs[here] = i;
            if (trace_input(c,w.connections[i],target,path)) return true;
        }
    }
    --path.count;
    return false;
}
}
bool input_path(codec_t* c, int pin, Path& path) {
    path = {};
    if (!input_pin(c,pin)) return false;
    for (int adc = c->wstart; adc < c->wend; ++adc) {
        const auto& w = c->w[adc];
        if (!w.enable || w.type != COP_AWTYPE_AUDIO_INPUT || (w.widgetcap & COP_AWCAP_DIGITAL) ||
            !(w.widgetcap & COP_AWCAP_STEREO) || !(w.d.audio.encodings & COP_STREAM_FORMAT_PCM) ||
            !(w.d.audio.bits_rates & COP_PCM_B16) || !(w.d.audio.bits_rates & COP_PCM_R480)) continue;
        path = {};
        if (trace_input(c,adc,pin,path)) return true;
    }
    path = {};
    return false;
}
bool find_input(codec_t* c, Path& best) {
    best = {};
    int score = -1;
    for (int pin = c->wstart; pin < c->wend; ++pin) {
        if (!input_pin(c,pin)) continue;
        const auto& w = c->w[pin];
        int rank = w.d.pin.device == CORB_CD_MICIN ? 200 : w.d.pin.device == CORB_CD_LINEIN ? 100 : 50;
        rank -= CORB_CD_SEQUENCE(w.d.pin.config);
        Path candidate;
        if (rank > score && input_path(c,pin,candidate)) { best = candidate; score = rank; }
    }
    return best.count != 0;
}
int configure_input_path(codec_t* c, const Path& path) {
    if (path.count < 2 || path.count > 10) return kInvalid;
    for (unsigned i = 0; i < path.count; ++i) {
        if (!azalia_widget_enabled(c,path.nodes[i])) return kInvalid;
        const auto& w = c->w[path.nodes[i]];
        const int input = path.inputs[i];
        if (w.type == COP_AWTYPE_PIN_COMPLEX) {
            if (i+1 != path.count || !input_pin(c,w.nid)) return kInvalid;
            // The pin's input amp is one microphone boost, not its output
            // connection list. Keep it at unity and mute its output amp.
            if ((w.widgetcap & COP_AWCAP_INAMP) && azalia_set_amp(c,w.nid,false,0,w.inamp_cap,false)) return kIoError;
            if ((w.widgetcap & COP_AWCAP_OUTAMP) && azalia_set_amp(c,w.nid,true,0,w.outamp_cap,true)) return kIoError;
            continue;
        }
        if (input < 0 || input >= w.nconnections) return kInvalid;
        if (w.type != COP_AWTYPE_AUDIO_MIXER &&
            azalia_comresp(c,w.nid,CORB_SET_CONNECTION_SELECT_CONTROL,input,nullptr)) return kIoError;
        if (w.widgetcap & COP_AWCAP_INAMP) {
            if (w.nconnections > 16) return kInvalid;
            for (int j = 0; j < w.nconnections; ++j)
                if (azalia_set_amp(c,w.nid,false,j,w.inamp_cap,j != input)) return kIoError;
        }
        if ((w.widgetcap & COP_AWCAP_OUTAMP) && azalia_set_amp(c,w.nid,true,0,w.outamp_cap,false)) return kIoError;
    }
    return azalia_widget_init_pin(&c->w[path.nodes[path.count-1]],c);
}
int switch_input(codec_t* c, Path& current, const Path& next) {
    if (!next.count || next.count > 10) return kInvalid;
    Path candidate;
    if (!input_path(c,next.nodes[next.count-1],candidate)) return kInvalid;
    if (current.count && current.nodes[current.count-1] == candidate.nodes[candidate.count-1]) return 0;
    const Path previous = current;
    int error = 0;
    if (previous.count) {
        error = azalia_comresp(c,previous.nodes[previous.count-1],CORB_SET_PIN_WIDGET_CONTROL,0,nullptr);
        if (!error) error = azalia_comresp(c,previous.nodes[0],CORB_SET_CONVERTER_STREAM_CHANNEL,0,nullptr);
    }
    for (int pin = c->wstart; !error && pin < c->wend; ++pin) {
        if (pin == candidate.nodes[candidate.count-1] ||
            (previous.count && pin == previous.nodes[previous.count-1])) continue;
        Path unused;
        if (input_path(c,pin,unused)) error = azalia_comresp(c,pin,CORB_SET_PIN_WIDGET_CONTROL,0,nullptr);
    }
    if (!error) error = configure_input_path(c,candidate);
    if (!error) { current = candidate; return 0; }
    const int quiet = azalia_comresp(c,candidate.nodes[candidate.count-1],CORB_SET_PIN_WIDGET_CONTROL,0,nullptr);
    if (quiet || (previous.count && configure_input_path(c,previous))) current = {};
    else current = previous;
    return kIoError;
}
} // namespace azalia_port
