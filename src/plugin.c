#include <lv2/core/lv2.h>
#include <lv2/atom/atom.h>
#include <lv2/atom/util.h>
#include <lv2/midi/midi.h>
#include <lv2/urid/urid.h>

#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <dirent.h>

#define PLUGIN_URI   "https://ton-nom.github.io/plugins/midi-clip-player"
#define MIDI_DIR     "/data/user-files/MIDI Songs"
#define MAX_CLIPS    16
#define MAX_EVENTS   2048

enum PortIndex {
    PORT_MIDI_IN     = 0,
    PORT_MIDI_OUT    = 1,
    PORT_CLIP_SELECT = 2,
    PORT_BPM         = 3
};

typedef struct {
    double  beat;
    uint8_t data[3];
    uint8_t size;
} MidiEvent;

typedef struct {
    MidiEvent events[MAX_EVENTS];
    int       event_count;
    double    length_beats;
    int       loaded;
    char      name[256];
} MidiClip;

typedef struct {
    LV2_URID_Map* map;
    LV2_URID      atom_Sequence;
    LV2_URID      midi_MidiEvent;

    const LV2_Atom_Sequence* midi_in;
    LV2_Atom_Sequence*       midi_out;
    const float*             clip_select_port;
    const float*             bpm_port;

    MidiClip clips[MAX_CLIPS];
    int      current_clip;
    int      next_clip;
    double   playhead_beats;
    double   sample_rate;
    int      last_clip_select;
    int      clip_count;
} Plugin;

static uint32_t read_uint(const uint8_t* buf, int n) {
    uint32_t v = 0;
    int i;
    for (i = 0; i < n; i++)
        v = (v << 8) | buf[i];
    return v;
}

static uint32_t read_vlq(const uint8_t* buf, int* bytes_read) {
    uint32_t v = 0;
    int i = 0;
    uint8_t b;
    do {
        b = buf[i++];
        v = (v << 7) | (b & 0x7F);
    } while (b & 0x80);
    *bytes_read = i;
    return v;
}

static int parse_midi_file(const char* path, MidiClip* clip) {
    FILE* f = fopen(path, "rb");
    if (!f) return 0;

    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);

    uint8_t* buf = (uint8_t*)malloc(fsize);
    if (!buf) { fclose(f); return 0; }
    fread(buf, 1, fsize, f);
    fclose(f);

    if (fsize < 14 || memcmp(buf, "MThd", 4) != 0) { free(buf); return 0; }

    uint16_t ppq = (uint16_t)read_uint(buf + 12, 2);
    if (ppq == 0) ppq = 96;

    long pos = 8 + (long)read_uint(buf + 4, 4);

    clip->event_count  = 0;
    clip->length_beats = 0.0;
    clip->loaded       = 0;

    uint32_t total_ticks = 0;
    uint8_t  running_status = 0;

    while (pos + 8 <= fsize) {
        if (memcmp(buf + pos, "MTrk", 4) != 0) { pos++; continue; }

        uint32_t track_len = read_uint(buf + pos + 4, 4);
        long track_start = pos + 8;
        long track_end   = track_start + track_len;
        if (track_end > fsize) break;

        long p = track_start;
        uint32_t tick = 0;

        while (p < track_end) {
            int vlen = 0;
            uint32_t delta = read_vlq(buf + p, &vlen);
            p += vlen;
            tick += delta;

            if (p >= track_end) break;

            uint8_t status = buf[p];

            if (status == 0xFF) {
                p++;
                if (p + 1 >= track_end) break;
                p++;
                int mlen = 0;
                uint32_t meta_len = read_vlq(buf + p, &mlen);
                p += mlen + meta_len;
                running_status = 0;
                continue;
            }

            if (status == 0xF0 || status == 0xF7) {
                p++;
                int slen = 0;
                uint32_t sysex_len = read_vlq(buf + p, &slen);
                p += slen + sysex_len;
                running_status = 0;
                continue;
            }

            uint8_t ev_status;
            if (status & 0x80) {
                ev_status      = status;
                running_status = status;
                p++;
            } else {
                ev_status = running_status;
            }

            uint8_t ev_type = ev_status & 0xF0;
            double  beat    = (double)tick / (double)ppq;

            if (ev_type == 0x90 || ev_type == 0x80) {
                if (p + 1 < track_end && clip->event_count < MAX_EVENTS) {
                    uint8_t note = buf[p];
                    uint8_t vel  = buf[p + 1];
                    uint8_t real_status = (ev_type == 0x90 && vel == 0) ?
                                          (0x80 | (ev_status & 0x0F)) : ev_status;
                    clip->events[clip->event_count].beat    = beat;
                    clip->events[clip->event_count].data[0] = real_status;
                    clip->events[clip->event_count].data[1] = note;
                    clip->events[clip->event_count].data[2] = vel;
                    clip->events[clip->event_count].size    = 3;
                    clip->event_count++;
                    if (beat > clip->length_beats) clip->length_beats = beat;
                }
                p += 2;
            } else if (ev_type == 0xA0 || ev_type == 0xB0 || ev_type == 0xE0) {
                p += 2;
            } else if (ev_type == 0xC0 || ev_type == 0xD0) {
                p += 1;
            } else {
                p++;
            }
        }

        if (tick > total_ticks) total_ticks = tick;
        pos = track_end;
    }

    if (clip->length_beats <= 0.0) clip->length_beats = 4.0;
    else clip->length_beats = ceil(clip->length_beats / 4.0) * 4.0;

    free(buf);
    clip->loaded = (clip->event_count > 0) ? 1 : 0;
    return clip->loaded;
}

static void scan_midi_files(Plugin* p) {
    DIR* dir = opendir(MIDI_DIR);
    if (!dir) return;

    struct dirent* entry;
    p->clip_count = 0;

    while ((entry = readdir(dir)) != NULL && p->clip_count < MAX_CLIPS) {
        const char* name = entry->d_name;
        size_t len = strlen(name);

        int is_mid  = (len > 4 && strcasecmp(name + len - 4, ".mid")  == 0);
        int is_midi = (len > 5 && strcasecmp(name + len - 5, ".midi") == 0);
        if (!is_mid && !is_midi) continue;

        char path[512];
        snprintf(path, sizeof(path), "%s/%s", MIDI_DIR, name);

        MidiClip* clip = &p->clips[p->clip_count];
        memset(clip, 0, sizeof(MidiClip));
        strncpy(clip->name, name, sizeof(clip->name) - 1);

        if (parse_midi_file(path, clip)) {
            p->clip_count++;
        }
    }

    closedir(dir);
}

static LV2_Handle instantiate(
    const LV2_Descriptor*     descriptor,
    double                    rate,
    const char*               bundle_path,
    const LV2_Feature* const* features)
{
    Plugin* p = (Plugin*)calloc(1, sizeof(Plugin));
    if (!p) return NULL;

    p->sample_rate      = rate;
    p->current_clip     = 0;
    p->next_clip        = -1;
    p->playhead_beats   = 0.0;
    p->last_clip_select = 0;
    p->clip_count       = 0;

    int i;
    for (i = 0; features[i]; ++i) {
        if (!strcmp(features[i]->URI, LV2_URID__map)) {
            p->map = (LV2_URID_Map*)features[i]->data;
            break;
        }
    }

    if (!p->map) { free(p); return NULL; }

    p->atom_Sequence  = p->map->map(p->map->handle, LV2_ATOM__Sequence);
    p->midi_MidiEvent = p->map->map(p->map->handle, LV2_MIDI__MidiEvent);

    scan_midi_files(p);

    if (p->clip_count == 0) {
        MidiClip* c = &p->clips[0];
        c->length_beats = 4.0;
        c->loaded = 1;
        c->event_count = 4;
        strncpy(c->name, "demo", sizeof(c->name)-1);
        double   beats[] = {0.0, 0.5, 1.0, 1.5};
        uint8_t  notes[] = {60,  60,  64,  64};
        uint8_t  stats[] = {0x90,0x80,0x90,0x80};
        uint8_t  vels[]  = {100, 0,   100, 0};
        for (i = 0; i < 4; i++) {
            c->events[i].beat    = beats[i];
            c->events[i].data[0] = stats[i];
            c->events[i].data[1] = notes[i];
            c->events[i].data[2] = vels[i];
            c->events[i].size    = 3;
        }
        p->clip_count = 1;
    }

    return (LV2_Handle)p;
}

static void connect_port(LV2_Handle instance, uint32_t port, void* data) {
    Plugin* p = (Plugin*)instance;
    switch (port) {
        case PORT_MIDI_IN:     p->midi_in         = (const LV2_Atom_Sequence*)data; break;
        case PORT_MIDI_OUT:    p->midi_out         = (LV2_Atom_Sequence*)data;       break;
        case PORT_CLIP_SELECT: p->clip_select_port = (const float*)data;             break;
        case PORT_BPM:         p->bpm_port         = (const float*)data;             break;
    }
}

static void activate(LV2_Handle instance) {
    Plugin* p = (Plugin*)instance;
    p->playhead_beats = 0.0;
    p->next_clip      = -1;
    if (p->clip_select_port) {
        p->current_clip     = (int)*p->clip_select_port;
        p->last_clip_select = p->current_clip;
    }
}

static void run(LV2_Handle instance, uint32_t n_samples) {
    Plugin* p = (Plugin*)instance;

    const float bpm         = *p->bpm_port;
    const int   clip_select = (int)*p->clip_select_port;

    /* Changement de clip : repart du début immédiatement */
    if (clip_select != p->last_clip_select) {
        p->last_clip_select = clip_select;
        if (clip_select >= 0 && clip_select < p->clip_count) {
            p->current_clip   = clip_select;
            p->next_clip      = -1;
            p->playhead_beats = 0.0;
        }
    }

    /* Lire MIDI entrant (Keystep) */
    LV2_ATOM_SEQUENCE_FOREACH(p->midi_in, ev) {
        if (ev->body.type == p->midi_MidiEvent) {
            const uint8_t* msg    = (const uint8_t*)(ev + 1);
            const uint8_t  status = msg[0] & 0xF0;
            if (status == 0xC0 && msg[1] < p->clip_count) {
                p->current_clip   = msg[1];
                p->next_clip      = -1;
                p->playhead_beats = 0.0;
                p->last_clip_select = msg[1];
            }
            if (status == 0x90 && msg[1] >= 36 && (msg[1]-36) < p->clip_count) {
                p->current_clip   = msg[1] - 36;
                p->next_clip      = -1;
                p->playhead_beats = 0.0;
                p->last_clip_select = msg[1] - 36;
            }
        }
    }

    /* Vider buffer sortie */
    lv2_atom_sequence_clear(p->midi_out);
    p->midi_out->atom.type = p->atom_Sequence;

    if (p->current_clip < 0 || p->current_clip >= p->clip_count) return;

    MidiClip* clip = &p->clips[p->current_clip];
    if (!clip->loaded || clip->event_count == 0) return;

    const double beats_per_second = (bpm > 0 ? bpm : 120.0) / 60.0;
    const double beats_per_sample = beats_per_second / p->sample_rate;
    const double beat_start       = p->playhead_beats;
    const double beat_end         = beat_start + n_samples * beats_per_sample;

    int i;
    for (i = 0; i < clip->event_count; i++) {
        double event_beat = clip->events[i].beat;
        while (event_beat < beat_start) event_beat += clip->length_beats;

        if (event_beat >= beat_start && event_beat < beat_end) {
            uint32_t offset = (uint32_t)((event_beat - beat_start) / beats_per_sample);
            if (offset >= n_samples) offset = n_samples - 1;

            uint32_t event_size = sizeof(LV2_Atom_Event) + 3;
            uint32_t padded     = (event_size + 7) & ~7;

            if (p->midi_out->atom.size + padded <= 4096) {
                LV2_Atom_Event* out_ev = (LV2_Atom_Event*)(
                    (uint8_t*)LV2_ATOM_CONTENTS(LV2_Atom_Sequence, p->midi_out)
                    + p->midi_out->atom.size - sizeof(LV2_Atom_Sequence_Body)
                );
                out_ev->time.frames = offset;
                out_ev->body.type   = p->midi_MidiEvent;
                out_ev->body.size   = clip->events[i].size;
                memcpy(out_ev + 1, clip->events[i].data, clip->events[i].size);
                p->midi_out->atom.size += padded;
            }
        }
    }

    p->playhead_beats += n_samples * beats_per_sample;

    /* Fin de clip : reboucle */
    if (p->playhead_beats >= clip->length_beats) {
        p->playhead_beats = fmod(p->playhead_beats, clip->length_beats);
    }
}

static void deactivate(LV2_Handle instance) {}
static void cleanup(LV2_Handle instance) { free(instance); }
static const void* extension_data(const char* uri) { return NULL; }

static const LV2_Descriptor descriptor = {
    PLUGIN_URI, instantiate, connect_port, activate,
    run, deactivate, cleanup, extension_data
};

LV2_SYMBOL_EXPORT const LV2_Descriptor* lv2_descriptor(uint32_t index) {
    return (index == 0) ? &descriptor : NULL;
}