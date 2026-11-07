/* Inspect operator state as well as PCM: these regressions can leave a song
 * audible and correctly pitched while changing its timbre. */
#include <stdio.h>
#include "../../subsystems/posix/sdk/v1/runtime/sxmidi.c"

static int failures;
#define CHECK(condition, message) do { \
    if (!(condition)) { printf("FAIL: %s\n", message); ++failures; } \
} while (0)

static struct sx_midi_voice held_voice(struct sx_midi_fm_patch* patch) {
    struct sx_midi_voice voice;
    memset(&voice, 0, sizeof(voice));
    voice.patch = patch;
    voice.active = 1;
    voice.fm_additive = 1;
    voice.fm_stage[0] = voice.fm_stage[1] = SX_MIDI_STAGE_SUSTAIN;
    voice.fm_mod_phase = 0x10000000u;
    voice.fm_car_phase = 0x30000000u;
    return voice;
}

int main(void) {
    struct sx_midi_fm_patch patch;
    struct sx_midi_voice voice, other;
    struct sx_midi_song song;
    struct sx_midi_event event;
    int32_t expected;
    memset(&patch, 0, sizeof(patch));
    sx_midi_bank_init();

    voice = held_voice(&patch);
    expected = sx_midi_fm_operator(voice.fm_mod_phase, 0) +
               sx_midi_fm_operator(voice.fm_car_phase, 0);
    CHECK(sx_midi_fm_voice_render(&voice) == expected,
          "additive output is the sum of unmodulated operators");

    voice = held_voice(&patch);
    voice.fm_stage[0] = voice.fm_stage[1] = SX_MIDI_STAGE_ATTACK;
    voice.fm_att[0] = voice.fm_att[1] = 100;
    voice.fm_attack_step[0] = 10;
    voice.fm_attack_step[1] = 100;
    voice.fm_sl[1] = 1000;
    voice.fm_decay_step[1] = 20;
    sx_midi_fm_envelope(&voice);
    sx_midi_fm_envelope(&voice);
    CHECK(voice.fm_att[0] == 80 && voice.fm_att[1] == 20,
          "modulator attack continues while carrier decays");

    voice = held_voice(&patch);
    voice.fm_stage[0] = SX_MIDI_STAGE_DECAY;
    voice.fm_sl[0] = 10;
    voice.fm_decay_step[0] = 10;
    voice.fm_release_step[0] = 3;
    sx_midi_fm_envelope(&voice);
    sx_midi_fm_envelope(&voice);
    CHECK(voice.fm_att[0] == 13,
          "percussive operator switches to release rate after sustain level");
    voice.fm_stage[1] = SX_MIDI_STAGE_RELEASE;
    voice.fm_att[1] = SX_MIDI_FM_ATT_FULL;
    sx_midi_fm_envelope(&voice);
    CHECK(voice.active, "additive modulator survives silent carrier");
    voice.stage = SX_MIDI_STAGE_RELEASE;
    voice.fm_att[0] = SX_MIDI_FM_ATT_FULL;
    sx_midi_fm_envelope(&voice);
    CHECK(!voice.active, "additive voice stops when both operators finish");

    voice = held_voice(&patch);
    voice.fm_level[0] = SX_MIDI_FM_ATT_LEVELS - 1;
    other = voice;
    voice.fm_am[0] = 1;
    voice.fm_am_phase = 0x40000000u;
    CHECK(sx_midi_fm_voice_render(&voice) == sx_midi_fm_voice_render(&other),
          "modulator tremolo does not attenuate carrier");
    voice = held_voice(&patch);
    voice.fm_level[1] = SX_MIDI_FM_ATT_LEVELS - 1;
    other = voice;
    voice.fm_am[1] = 1;
    voice.fm_am_phase = 0x40000000u;
    CHECK(sx_midi_fm_voice_render(&voice) == sx_midi_fm_voice_render(&other),
          "carrier tremolo does not attenuate modulator");
    voice = held_voice(&patch);
    voice.fm_vib[0] = 1;
    voice.fm_vib_phase = 0x40000000u;
    voice.fm_mod_step = voice.fm_car_step = 1000000;
    sx_midi_fm_voice_render(&voice);
    CHECK(voice.fm_car_phase == 0x30000000u + 1000000 &&
          voice.fm_mod_phase != 0x10000000u + 1000000,
          "modulator vibrato leaves carrier frequency unchanged");
    voice = held_voice(&patch);
    voice.fm_vib[1] = 1;
    voice.fm_vib_phase = 0x40000000u;
    voice.fm_mod_step = voice.fm_car_step = 1000000;
    sx_midi_fm_voice_render(&voice);
    CHECK(voice.fm_mod_phase == 0x10000000u + 1000000 &&
          voice.fm_car_phase != 0x30000000u + 1000000,
          "carrier vibrato leaves modulator frequency unchanged");

    memset(&song, 0, sizeof(song));
    song.voices[0] = held_voice(&patch);
    song.voices[0].fm_velocity = 127;
    song.channel[0].volume = 127;
    sx_midi_fm_voice_volume(&song, &song.voices[0]);
    CHECK(song.voices[0].fm_level[1] == 0, "full volume opens carrier");
    memset(&event, 0, sizeof(event));
    event.kind = SX_MIDI_EVENT_CONTROL;
    event.a = 7;
    sx_midi_apply_event(&song, &event);
    CHECK(song.voices[0].fm_level[0] == 63 && song.voices[0].fm_level[1] == 63,
          "live channel volume attenuates both additive operators");
    song.voices[0].fm_additive = 0;
    sx_midi_fm_voice_volume(&song, &song.voices[0]);
    CHECK(song.voices[0].fm_level[0] == patch.mod[0].level,
          "FM modulator keeps instrument level when channel volume changes");
    printf(failures ? "FM TEST FAIL\n" : "FM TEST PASS\n");
    return failures != 0;
}
