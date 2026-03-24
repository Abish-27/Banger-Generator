#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

// GM drum note numbers
#define KICK       36
#define CLAP       39   // hand clap
#define HIHAT      42
#define HIHAT_OPEN 46   // open hi-hat
#define CRASH      49   // crash cymbal
#define COWBELL    56   // THE phonk sound

#define TICKS_PER_BEAT  480
#define TICKS_16TH      (TICKS_PER_BEAT / 4)   // 120 ticks
#define TICKS_32ND      (TICKS_PER_BEAT / 8)   // 60 ticks — for hi-hat rolls
#define NOTE_DURATION   60
#define BARS            4

typedef struct {
    uint32_t tick;
    uint8_t  note;
    uint8_t  vel;   // 0 = note off
} Event;

// --- MIDI write helpers ---

static void write_u16(FILE *f, uint16_t v) {
    fputc((v >> 8) & 0xFF, f);
    fputc(v & 0xFF, f);
}

static void write_u32(FILE *f, uint32_t v) {
    fputc((v >> 24) & 0xFF, f);
    fputc((v >> 16) & 0xFF, f);
    fputc((v >>  8) & 0xFF, f);
    fputc( v        & 0xFF, f);
}

// Variable-length quantity encoding (MIDI delta times)
static void write_vlq(FILE *f, uint32_t v) {
    uint8_t buf[4];
    int len = 0;
    buf[len++] = v & 0x7F;
    v >>= 7;
    while (v) {
        buf[len++] = (v & 0x7F) | 0x80;
        v >>= 7;
    }
    for (int i = len - 1; i >= 0; i--)
        fputc(buf[i], f);
}

// --- Event helpers ---

static void push(Event *evs, int *n, uint32_t tick, uint8_t note, uint8_t vel) {
    evs[(*n)++] = (Event){tick, note, vel};
}

static int cmp_event(const void *a, const void *b) {
    const Event *ea = a, *eb = b;
    if (ea->tick != eb->tick) return (ea->tick > eb->tick) - (ea->tick < eb->tick);
    // note-offs before note-ons at same tick to avoid stuck notes
    return (ea->vel > 0) - (eb->vel > 0);
}

int main(void) {
    // Build event list
    Event evs[1024];
    int n = 0;

    for (int bar = 0; bar < BARS; bar++) {
        // Sigma phonk — 140 BPM, 16th note grid
        // Kick:    galloping double-kick pattern (0,2,3 | 8,10,11)
        // Clap:    4, 12 (beats 2+4), layered with crash on bar 1 beat 1
        // Cowbell: offbeats (1,3,5,7,9,11,13,15) — dark phonk pulse
        // Hi-hat:  all 16 steps, loud on beats, ghost on offbeats
        // 32nd rolls: rapid HH pairs at step 6 and 14 for tension
        static const int kick_steps[] = {0, 2, 3, 8, 10, 11};
        static const int clap_steps[] = {4, 12};

        for (int step = 0; step < 16; step++) {
            uint32_t t = (uint32_t)((bar * 16 + step) * TICKS_16TH);

            // Hi-hat: accent on beats (0,4,8,12), medium on 8ths, ghost on 16ths
            uint8_t hh_vel = (step % 4 == 0) ? 110 : (step % 2 == 0) ? 70 : 40;
            push(evs, &n, t,                 HIHAT, hh_vel);
            push(evs, &n, t + NOTE_DURATION, HIHAT, 0);

            // 32nd note hi-hat roll: extra hit 1 32nd before steps 6 and 14
            if (step == 6 || step == 14) {
                uint32_t roll = t - TICKS_32ND;
                push(evs, &n, roll,                  HIHAT, 55);
                push(evs, &n, roll + NOTE_DURATION,  HIHAT, 0);
                // open hi-hat ON the step itself for the "sss" effect
                push(evs, &n, t,                 HIHAT_OPEN, 90);
                push(evs, &n, t + NOTE_DURATION, HIHAT_OPEN, 0);
            }

            // Cowbell on every offbeat 16th (the dark phonk pulse)
            if (step % 2 == 1) {
                push(evs, &n, t,                 COWBELL, 80);
                push(evs, &n, t + NOTE_DURATION, COWBELL, 0);
            }

            // Kick — galloping double kick
            for (int k = 0; k < 6; k++) {
                if (step == kick_steps[k]) {
                    uint8_t kvel = (step == 0 || step == 8) ? 127 : 100;
                    push(evs, &n, t,                 KICK, kvel);
                    push(evs, &n, t + NOTE_DURATION, KICK, 0);
                }
            }

            // Clap
            for (int c = 0; c < 2; c++) {
                if (step == clap_steps[c]) {
                    push(evs, &n, t,                 CLAP, 120);
                    push(evs, &n, t + NOTE_DURATION, CLAP, 0);
                }
            }
        }

        // Crash on beat 1 of every bar for that hard drop feel
        uint32_t bar_start = (uint32_t)(bar * 16 * TICKS_16TH);
        push(evs, &n, bar_start,                 CRASH, 100);
        push(evs, &n, bar_start + NOTE_DURATION, CRASH, 0);
    }

    qsort(evs, n, sizeof(Event), cmp_event);

    // Write track data to a temp buffer (need byte count before writing MTrk header)
    FILE *tmp = tmpfile();
    if (!tmp) { perror("tmpfile"); return 1; }

    // Tempo meta event: 140 BPM = 428571 microseconds/beat
    fputc(0x00, tmp);  // delta = 0
    fputc(0xFF, tmp);  // meta event
    fputc(0x51, tmp);  // set tempo
    fputc(0x03, tmp);  // 3 bytes follow
    fputc(0x06, tmp);  // 428571 = 0x068A1B
    fputc(0x8A, tmp);
    fputc(0x1B, tmp);

    // Write note events
    uint32_t prev = 0;
    for (int i = 0; i < n; i++) {
        write_vlq(tmp, evs[i].tick - prev);
        prev = evs[i].tick;
        fputc(evs[i].vel ? 0x99 : 0x89, tmp);  // note on/off, channel 9 (drums)
        fputc(evs[i].note, tmp);
        fputc(evs[i].vel, tmp);
    }

    // End of track
    fputc(0x00, tmp);
    fputc(0xFF, tmp);
    fputc(0x2F, tmp);
    fputc(0x00, tmp);

    long track_len = ftell(tmp);

    // Now write the actual MIDI file
    FILE *f = fopen("drums.mid", "wb");
    if (!f) { perror("fopen"); return 1; }

    // MThd chunk
    fwrite("MThd", 1, 4, f);
    write_u32(f, 6);           // header length always 6
    write_u16(f, 0);           // format 0 = single track
    write_u16(f, 1);           // 1 track
    write_u16(f, TICKS_PER_BEAT);

    // MTrk chunk
    fwrite("MTrk", 1, 4, f);
    write_u32(f, (uint32_t)track_len);

    // Copy track data from temp file
    rewind(tmp);
    char buf[4096];
    size_t r;
    while ((r = fread(buf, 1, sizeof(buf), tmp)) > 0)
        fwrite(buf, 1, r, f);

    fclose(tmp);
    fclose(f);

    printf("drums.mid written (%ld track bytes, %d events)\n", track_len, n);
    printf("Playing with timidity...\n");
    fflush(stdout);

    return system("timidity drums.mid");
}
