// Playback adaptation of OpenBSD azalia.c and azalia_codec.c. The upstream
// BSD-2-Clause notice is retained below; see docs/kernel/AZALIA.md for scope.
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
int parameter(const codec_t* codec, int nid, unsigned param, uint32_t& value) {
    return azalia_comresp(codec, nid, CORB_GET_PARAMETER, param, &value);
}
}

// OpenBSD azalia_get_ctrlr_caps: retain the capability/selection split and
// prefer 256, 16, then 2. DMA is supplied by SavanXP before this call.
int azalia_init_rings(azalia_t* az) {
    auto read = [&](uint16_t reg, unsigned width) { return az->io.read(az->io.context, reg, width); };
    auto write = [&](uint16_t reg, uint32_t value, unsigned width) {
        az->io.write(az->io.context, reg, value, width);
    };
    auto wait = [&](uint16_t reg, uint32_t mask, bool set, unsigned width) {
        for (unsigned i = 0; i < 5000; ++i) {
            if (((read(reg, width) & mask) != 0) == set) return true;
            az->io.delay(az->io.context, 10);
        }
        return false;
    };
    auto size = [&](uint16_t reg, uint8_t& selected, unsigned& entries) {
        const uint8_t cap = read(reg, 1);
        if (cap & HDA_CORBSIZE_CORBSZCAP_256) {
            entries = 256; selected = HDA_CORBSIZE_CORBSIZE_256;
        } else if (cap & HDA_CORBSIZE_CORBSZCAP_16) {
            entries = 16; selected = HDA_CORBSIZE_CORBSIZE_16;
        } else if (cap & HDA_CORBSIZE_CORBSZCAP_2) {
            entries = 2; selected = HDA_CORBSIZE_CORBSIZE_2;
        } else return false;
        return true;
    };
    if (!size(HDA_CORBSIZE, az->corbsize, az->corb_entries) ||
        !size(HDA_RIRBSIZE, az->rirbsize, az->rirb_entries)) return kInvalid;

    // azalia_init_corb / azalia_init_rirb, without autoconf or timeout(9).
    write(HDA_INTCTL, 0, 4);
    write(HDA_CORBCTL, 0, 1);
    write(HDA_RIRBCTL, 0, 1);
    if (!wait(HDA_CORBCTL, HDA_CORBCTL_CORBRUN, false, 1) ||
        !wait(HDA_RIRBCTL, HDA_RIRBCTL_RIRBDMAEN, false, 1)) return kIoError;
    write(HDA_CORBLBASE, az->corb_dma.physical, 4);
    write(HDA_CORBUBASE, az->corb_dma.physical >> 32, 4);
    write(HDA_RIRBLBASE, az->rirb_dma.physical, 4);
    write(HDA_RIRBUBASE, az->rirb_dma.physical >> 32, 4);
    write(HDA_CORBSIZE, az->corbsize, 1);
    write(HDA_RIRBSIZE, az->rirbsize, 1);
    write(HDA_CORBRP, HDA_CORBRP_CORBRPRST, 2);
    write(HDA_CORBRP, 0, 2);
    if (!wait(HDA_CORBRP, HDA_CORBRP_CORBRPRST, false, 2)) return kIoError;
    write(HDA_CORBWP, 0, 2);
    write(HDA_RIRBWP, HDA_RIRBWP_RIRBWPRST, 2);
    az->rirb_rp = read(HDA_RIRBWP, 2) & HDA_RIRBWP_RIRBWP;
    write(HDA_RINTCNT, 1, 2);
    write(HDA_RIRBSTS, HDA_RIRBSTS_RINTFL | HDA_RIRBSTS_RIRBOIS, 1);
    write(HDA_DPLBASE, 0, 4);
    write(HDA_DPUBASE, 0, 4);
    write(HDA_RIRBCTL, HDA_RIRBCTL_RIRBDMAEN | HDA_RIRBCTL_RINTCTL, 1);
    write(HDA_CORBCTL, HDA_CORBCTL_CORBRUN, 1);
    return wait(HDA_RIRBCTL, HDA_RIRBCTL_RIRBDMAEN, true, 1) &&
        wait(HDA_CORBCTL, HDA_CORBCTL_CORBRUN, true, 1) ? 0 : kIoError;
}

int azalia_comresp(const codec_t* codec, nid_t nid, uint32_t control,
                   uint32_t param, uint32_t* result) {
    int err = azalia_set_command(codec->az, codec->address, nid, control, param);
    if (err) return err;
    return azalia_get_response(codec->az, result);
}

// azalia_widget_init_connection with bounded storage and separate encoded and
// expanded indices. Both short/long entries and range expansion are retained.
int read_connections(widget_t* w, const codec_t* codec) {
    w->nconnections = 0;
    w->selected = -1;
    if (!(w->widgetcap & COP_AWCAP_CONNLIST)) return 0;
    uint32_t result;
    if (parameter(codec, w->nid, COP_CONNECTION_LIST_LENGTH, result)) return kIoError;
    const unsigned bits = (result & COP_CLL_LONG) ? 16 : 8;
    const unsigned length = COP_CLL_LENGTH(result);
    const unsigned range_bit = 1u << (bits - 1);
    unsigned last = 0;
    for (unsigned encoded = 0; encoded < length;) {
        if (azalia_comresp(codec, w->nid, CORB_GET_CONNECTION_LIST_ENTRY,
                          encoded, &result)) return kIoError;
        for (unsigned k = 0; k < 32 / bits && encoded < length; ++k, ++encoded) {
            const unsigned entry = (result >> (k * bits)) & ((1u << bits) - 1);
            const unsigned node = entry & ~range_bit;
            if (node >= kNodes || node == 0) return kInvalid;
            unsigned first = node;
            if (entry & range_bit) {
                if (!w->nconnections || node <= last) return kInvalid;
                first = last + 1;
            }
            for (unsigned next = first; next <= node; ++next) {
                if (static_cast<unsigned>(w->nconnections) == kConnections) return kInvalid;
                w->connections[w->nconnections++] = next;
            }
            last = node;
        }
    }
    if (length) {
        if (azalia_comresp(codec, w->nid, CORB_GET_CONNECTION_SELECT_CONTROL, 0, &result)) return kIoError;
        w->selected = CORB_CSC_INDEX(result);
    }
    return 0;
}

// azalia_codec_init's function/widget discovery and base-parameter inheritance.
// audio(9) mixer construction, ADC groups and the broad quirk table are deferred.
int probe_codec(codec_t* c) {
    uint32_t result;
    if (parameter(c, CORB_NID_ROOT, COP_VENDOR_ID, c->vid) ||
        parameter(c, CORB_NID_ROOT, COP_SUBORDINATE_NODE_COUNT, result)) return kIoError;
    const unsigned first = COP_START_NID(result), count = COP_NSUBNODES(result);
    if (first + count > kNodes) return kInvalid;
    for (unsigned i = first; i < first + count; ++i) {
        if (!parameter(c, i, COP_FUNCTION_GROUP_TYPE, result) && COP_FTYPE(result) == COP_FTYPE_AUDIO) {
            c->audiofunc = i; break;
        }
    }
    if (c->audiofunc < 0) return kInvalid;
    if (azalia_comresp(c, c->audiofunc, CORB_SET_POWER_STATE, CORB_PS_D0, nullptr)) return kIoError;
    c->az->io.delay(c->az->io.context, 1000);
    if (parameter(c, c->audiofunc, COP_SUBORDINATE_NODE_COUNT, result)) return kIoError;
    c->wstart = COP_START_NID(result);
    c->wend = c->wstart + COP_NSUBNODES(result);
    if (c->wstart < 2 || c->wend > static_cast<int>(kNodes) || c->wstart >= c->wend) return kInvalid;
    widget_t& afg = c->w[c->audiofunc];
    if (parameter(c, c->audiofunc, COP_STREAM_FORMATS, afg.d.audio.encodings) ||
        parameter(c, c->audiofunc, COP_PCM, afg.d.audio.bits_rates) ||
        parameter(c, c->audiofunc, COP_INPUT_AMPCAP, afg.inamp_cap) ||
        parameter(c, c->audiofunc, COP_OUTPUT_AMPCAP, afg.outamp_cap)) return kIoError;
    for (int i = c->wstart; i < c->wend; ++i) {
        widget_t& w = c->w[i];
        w.nid = i; w.enable = 1;
        if (parameter(c, i, COP_AUDIO_WIDGET_CAP, w.widgetcap)) return kIoError;
        w.type = COP_AWCAP_TYPE(w.widgetcap);
        if (w.widgetcap & COP_AWCAP_POWER) {
            if (azalia_comresp(c, i, CORB_SET_POWER_STATE, CORB_PS_D0, nullptr)) return kIoError;
            c->az->io.delay(c->az->io.context, 100);
        }
        if (w.widgetcap & COP_AWCAP_AMPOV) {
            if ((w.widgetcap & COP_AWCAP_INAMP) && parameter(c, i, COP_INPUT_AMPCAP, w.inamp_cap)) return kIoError;
            if ((w.widgetcap & COP_AWCAP_OUTAMP) && parameter(c, i, COP_OUTPUT_AMPCAP, w.outamp_cap)) return kIoError;
        } else { w.inamp_cap = afg.inamp_cap; w.outamp_cap = afg.outamp_cap; }
        if ((w.type == COP_AWTYPE_AUDIO_OUTPUT || w.type == COP_AWTYPE_AUDIO_INPUT) &&
            azalia_widget_init_audio(&w, c)) return kIoError;
        // Read pin properties here, but configure only the selected analog pin.
        // Upstream init_pin (including EAPD) is called after route selection.
        if (w.type == COP_AWTYPE_PIN_COMPLEX) {
            if (azalia_comresp(c, i, CORB_GET_CONFIGURATION_DEFAULT, 0, &w.d.pin.config) ||
                parameter(c, i, COP_PINCAP, w.d.pin.cap)) return kIoError;
            w.d.pin.device = CORB_CD_DEVICE(w.d.pin.config);
            if (CORB_CD_PORT(w.d.pin.config) == CORB_CD_NONE) w.enable = 0;
        }
        if (read_connections(&w, c)) return kIoError;
    }
    return 0;
}

namespace {
bool trace(codec_t* c, int node, Path& path) {
    if (!azalia_widget_enabled(c, node) || path.count == 10) return false;
    for (unsigned i = 0; i < path.count; ++i) if (path.nodes[i] == node) return false;
    const auto& w = c->w[node];
    if (w.widgetcap & COP_AWCAP_DIGITAL) return false;
    path.nodes[path.count] = node;
    path.inputs[path.count++] = -1;
    if (w.type == COP_AWTYPE_AUDIO_OUTPUT) {
        const bool pcm = (w.d.audio.encodings & COP_STREAM_FORMAT_PCM) &&
            (w.d.audio.bits_rates & COP_PCM_B16) && (w.d.audio.bits_rates & COP_PCM_R480) &&
            (w.widgetcap & COP_AWCAP_STEREO);
        if (pcm) return true;
    } else if (path.count == 1 || w.type == COP_AWTYPE_AUDIO_MIXER || w.type == COP_AWTYPE_AUDIO_SELECTOR) {
        const unsigned here = path.count - 1;
        // Respect a valid firmware selector first. On ALC892 the front pin
        // can reach four DAC mixers and commonly selects DAC 0x05 (index 3).
        const int preferred = w.type != COP_AWTYPE_AUDIO_MIXER && w.selected >= 0 &&
            w.selected < w.nconnections ? w.selected : -1;
        if (preferred >= 0) {
            path.inputs[here] = preferred;
            if (trace(c, w.connections[preferred], path)) return true;
        }
        for (int i = 0; i < w.nconnections; ++i) {
            if (i == preferred) continue;
            path.inputs[here] = i;
            if (trace(c, w.connections[i], path)) return true;
        }
    }
    --path.count;
    return false;
}
int amp(codec_t* c, int nid, bool output, unsigned input, uint32_t cap, bool mute) {
    // OpenBSD mixer_set payload: the same verb selects direction and both
    // channels in the payload. Even zero-step (mute-only) amplifiers need it.
    unsigned gain = COP_AMPCAP_OFFSET(cap);
    const unsigned steps = COP_AMPCAP_NUMSTEPS(cap);
    if (gain > steps) gain = steps;
    const uint32_t payload = (output ? CORB_AGM_OUTPUT : CORB_AGM_INPUT) |
        CORB_AGM_LEFT | CORB_AGM_RIGHT | (input << CORB_AGM_INDEX_SHIFT) |
        gain | ((mute && (cap & COP_AMPCAP_MUTE)) ? CORB_AGM_MUTE : 0);
    return azalia_comresp(c, nid, CORB_SET_AMPLIFIER_GAIN_MUTE, payload, nullptr);
}
}

int azalia_set_amp(codec_t* c, int nid, bool output, unsigned input, uint32_t cap, bool mute) {
    return amp(c,nid,output,input,cap,mute);
}

bool output_path(codec_t* c, int pin, Path& path) {
    path = {};
    if (!azalia_widget_enabled(c, pin)) return false;
    const auto& w = c->w[pin];
    if (w.type != COP_AWTYPE_PIN_COMPLEX ||
        (w.widgetcap & COP_AWCAP_DIGITAL) || !(w.d.pin.cap & COP_PINCAP_OUTPUT) ||
        (w.d.pin.cap & COP_PINCAP_HDMI)) return false;
    const unsigned device = CORB_CD_DEVICE(w.d.pin.config);
    if (device != CORB_CD_SPEAKER && device != CORB_CD_LINEOUT && device != CORB_CD_HEADPHONE) return false;
    return trace(c, pin, path);
}

bool find_output(codec_t* c, Path& best) {
    int score = -1;
    best = {};
    // Follow OpenBSD's analog preference and firmware port/association data.
    // A rear line-out is preferred to front headphones for the host target.
    for (int i = c->wstart; i < c->wend; ++i) {
        const auto& w = c->w[i];
        if (!w.enable || w.type != COP_AWTYPE_PIN_COMPLEX ||
            (w.widgetcap & COP_AWCAP_DIGITAL) || !(w.d.pin.cap & COP_PINCAP_OUTPUT) ||
            (w.d.pin.cap & COP_PINCAP_HDMI)) continue;
        const unsigned device = CORB_CD_DEVICE(w.d.pin.config);
        int rank = device == CORB_CD_SPEAKER ? 300 : device == CORB_CD_LINEOUT ? 200 :
            device == CORB_CD_HEADPHONE ? 100 : -1;
        if (rank < 0) continue;
        rank -= CORB_CD_SEQUENCE(w.d.pin.config);
        if (rank <= score) continue;
        Path candidate;
        if (trace(c, i, candidate)) { best = candidate; score = rank; }
    }
    return best.count != 0;
}

int configure_path(codec_t* c, const Path& path) {
    if (!path.count) return kInvalid;
    for (unsigned i = 0; i < path.count; ++i) {
        const int nid = path.nodes[i];
        const auto& w = c->w[nid];
        const int input = path.inputs[i];
        if (input >= 0 && w.type != COP_AWTYPE_AUDIO_MIXER) {
            if (azalia_comresp(c, nid, CORB_SET_CONNECTION_SELECT_CONTROL, input, nullptr)) return kIoError;
        }
        // A bidirectional pin's input amp is its microphone boost, not a
        // per-connection playback mixer. Leave it alone on an output route.
        if ((w.widgetcap & COP_AWCAP_INAMP) && w.type != COP_AWTYPE_PIN_COMPLEX) {
            if (w.nconnections > 16) return kInvalid; // amplifier index is four bits
            for (int j = 0; j < w.nconnections; ++j)
                if (amp(c, nid, false, j, w.inamp_cap, j != input)) return kIoError;
        }
        if ((w.widgetcap & COP_AWCAP_OUTAMP) && amp(c, nid, true, 0, w.outamp_cap, false)) return kIoError;
    }
    widget_t& pin = c->w[path.nodes[0]];
    int err = azalia_widget_init_pin(&pin, c);
    if (err) return err;
    if (pin.d.pin.device == CORB_CD_HEADPHONE && (pin.d.pin.cap & COP_PINCAP_HEADPHONE))
        return azalia_comresp(c, pin.nid, CORB_SET_PIN_WIDGET_CONTROL,
                             CORB_PWC_OUTPUT | CORB_PWC_HEADPHONE, nullptr);
    return 0;
}

bool pin_detectable(const widget_t& pin) {
    // Same capability and firmware override rules as OpenBSD sense_pins.
    return pin.enable && pin.type == COP_AWTYPE_PIN_COMPLEX &&
        CORB_CD_PORT(pin.d.pin.config) == CORB_CD_JACK &&
        (pin.d.pin.cap & COP_PINCAP_PRESENCE) &&
        !(CORB_CD_MISC(pin.d.pin.config) & CORB_CD_PRESENCEOV);
}
PinPresence pin_presence(codec_t* c, int nid) {
    if (!azalia_widget_enabled(c,nid) || !pin_detectable(c->w[nid])) return PinPresence::Unsupported;
    uint32_t sense = 0;
    if ((c->w[nid].d.pin.cap & COP_PINCAP_TRIGGER) &&
        azalia_comresp(c,nid,CORB_EXECUTE_PIN_SENSE,0,nullptr)) return PinPresence::Unknown;
    if (azalia_comresp(c,nid,CORB_GET_PIN_SENSE,0,&sense)) return PinPresence::Unknown;
    return (sense & CORB_PS_PRESENCE) ? PinPresence::Present : PinPresence::Absent;
}

int switch_output(codec_t* c, Path& current, const Path& next) {
    // Rebuild from the pin rather than trusting a caller-supplied path.
    Path candidate;
    if (!next.count || !output_path(c, next.nodes[0], candidate)) return kInvalid;
    if (current.count && current.nodes[0] == candidate.nodes[0]) return 0;
    const Path previous = current;
    int err = 0;
    if (previous.count) {
        err = azalia_comresp(c, previous.nodes[0], CORB_SET_PIN_WIDGET_CONTROL, 0, nullptr);
        if (!err) err = azalia_comresp(c, previous.nodes[previous.count - 1],
                                      CORB_SET_CONVERTER_STREAM_CHANNEL, 0, nullptr);
    }
    for (int nid = c->wstart; !err && nid < c->wend; ++nid) {
        if (nid == candidate.nodes[0] || (previous.count && nid == previous.nodes[0])) continue;
        Path unused;
        if (output_path(c,nid,unused))
            err = azalia_comresp(c,nid,CORB_SET_PIN_WIDGET_CONTROL,0,nullptr);
    }
    if (!err) err = configure_path(c, candidate);
    if (!err) { current = candidate; return 0; }
    // The candidate may have been partially enabled. Silence it, then restore
    // every old selector and amp, including shared widgets affected above.
    const int quiet = azalia_comresp(c, candidate.nodes[0], CORB_SET_PIN_WIDGET_CONTROL, 0, nullptr);
    if (quiet || (previous.count && configure_path(c, previous))) current = {};
    else current = previous;
    return kIoError;
}

int azalia_codec_connect_stream(stream_t* s) {
    if (azalia_comresp(s->codec, s->converter, CORB_SET_CONVERTER_FORMAT, s->fmt, nullptr)) return kIoError;
    return azalia_comresp(s->codec, s->converter, CORB_SET_CONVERTER_STREAM_CHANNEL,
                         s->number << 4, nullptr);
}
} // namespace azalia_port
