#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

// GM drum note numbers
#define KICK   36
#define SNARE  38
#define HIHAT  42

#define TICKS_PER_BEAT 480
#define TICKS_8TH      (TICKS_PER_BEAT / 2)  // 240
#define NOTE_DURATION  100  // ticks a note stays "on"
#define BARS           4

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
        for (int step = 0; step < 8; step++) {  // 8 eighth-notes per bar
            uint32_t t = (uint32_t)((bar * 8 + step) * TICKS_8TH);

            // Hi-hat every 8th note
            push(evs, &n, t,                  HIHAT, 70);
            push(evs, &n, t + NOTE_DURATION,  HIHAT, 0);

            // Kick on beats 1 and 3 (steps 0, 4)
            if (step == 0 || step == 4) {
                push(evs, &n, t,                 KICK, 110);
                push(evs, &n, t + NOTE_DURATION, KICK, 0);
            }

            // Snare on beats 2 and 4 (steps 2, 6)
            if (step == 2 || step == 6) {
                push(evs, &n, t,                 SNARE, 95);
                push(evs, &n, t + NOTE_DURATION, SNARE, 0);
            }
        }
    }

    qsort(evs, n, sizeof(Event), cmp_event);

    // Write track data to a temp buffer (need byte count before writing MTrk header)
    FILE *tmp = tmpfile();
    if (!tmp) { perror("tmpfile"); return 1; }

    // Tempo meta event: 120 BPM = 500000 microseconds/beat
    fputc(0x00, tmp);  // delta = 0
    fputc(0xFF, tmp);  // meta event
    fputc(0x51, tmp);  // set tempo
    fputc(0x03, tmp);  // 3 bytes follow
    fputc(0x07, tmp);  // 500000 = 0x07A120
    fputc(0xA1, tmp);
    fputc(0x20, tmp);

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
