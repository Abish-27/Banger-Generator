/*
 * midi_gen.c
 *
 * A MIDI song generator that uses pipes and multiple child processes.
 *
 * The parent process reads the user's song settings, then starts three
 * worker processes to build the drums, guitar, and piano parts in parallel.
 * It sends each worker the song information through a pipe, reads the
 * finished track data back through another pipe, and combines everything
 * into one Standard MIDI File.
 *
 * Pipe setup for each worker:
 *   to_child[i]   - parent sends song settings to the child
 *   from_child[i] - child sends generated track data back to the parent
 *
 * Output:
 *   output.mid
 *   MIDI format 1 with four tracks total:
 *   one tempo track, plus drums, guitar, and piano
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <stdint.h>
#include <errno.h>
#include <time.h>

/* ==================== Constants ==================== */

#define NUM_WORKERS 3
#define OUTPUT_FILE "output.mid"
#define TPQ 480 /* ticks per quarter note */
#define BEAT TPQ
#define EIGHT (TPQ / 2)
#define BEATS_PER_BAR 4

#define MAX_DRUM_EVENTS 8192
#define MAX_GUITAR_EVENTS 8192
#define MAX_PIANO_EVENTS 4096

/* MIDI channel assignments */
#define CH_PIANO 0
#define CH_GUITAR 1
#define CH_DRUMS 9 /* standard GM percussion channel */

/* ==================== Data types ==================== */

typedef enum
{
    WORKER_DRUMS = 0,
    WORKER_GUITAR = 1,
    WORKER_PIANO = 2
} WorkerRole;

typedef enum
{
    GENRE_POP = 0,
    GENRE_LOFI = 1,
    GENRE_ROCK = 2,
    GENRE_LOONY_TUNES = 3
} Genre;

/* read by child as raw bytes over pipe */
typedef struct
{
    int bpm;
    int bars;
    int key_root; /* MIDI note number, e.g. C4 = 60 */
    Genre genre;
    int lofi_prog; /* chosen once per song when genre=lofi */
    int rock_prog; /* chosen once per song when genre=rock */
    int pop_prog;  /* chosen once per song when genre=pop */
    WorkerRole role;
} SongSpec;

/* One complete MIDI track received from child */
typedef struct
{
    uint8_t *data;
    size_t len;
} TrackBuf;

/* Dynamic byte buffer */
typedef struct
{
    uint8_t *data;
    size_t len;
    size_t cap;
} DynBuf;

/* ==================== I/O helpers ==================== */

/* Error-checked write */
static void safe_write(int fd, const void *buf, size_t n, const char *ctx)
{
    size_t done = 0;
    while (done < n)
    {
        ssize_t w = write(fd, (const uint8_t *)buf + done, n - done);
        if (w < 0)
        {
            fprintf(stderr, "[ERROR] write(%s): %s\n", ctx, strerror(errno));
            exit(EXIT_FAILURE);
        }
        done += (size_t)w;
    }
}

/* Error-checked read */
static void safe_read_exact(int fd, void *buf, size_t n, const char *ctx)
{
    size_t done = 0;
    while (done < n)
    {
        ssize_t r = read(fd, (uint8_t *)buf + done, n - done);
        if (r < 0)
        {
            fprintf(stderr, "[ERROR] read(%s): %s\n", ctx, strerror(errno));
            exit(EXIT_FAILURE);
        }
        if (r == 0)
        {
            fprintf(stderr, "[ERROR] unexpected EOF in read(%s) after %zu/%zu bytes\n",
                    ctx, done, n);
            exit(EXIT_FAILURE);
        }
        done += (size_t)r;
    }
}

/* ==================== DynBuf helpers ==================== */

/* Initialize a dynamic buffer */
static void db_init(DynBuf *db)
{
    db->cap = 512;
    db->len = 0;
    db->data = malloc(db->cap);
    if (!db->data)
    {
        perror("malloc DynBuf");
        exit(EXIT_FAILURE);
    }
}

/* Push bytes into a dynamic buffer */
static void db_push(DynBuf *db, const uint8_t *bytes, size_t n)
{
    if (db->len + n > db->cap)
    {
        while (db->len + n > db->cap)
            db->cap *= 2;
        uint8_t *tmp = realloc(db->data, db->cap);
        if (!tmp)
        {
            perror("realloc DynBuf");
            free(db->data);
            exit(EXIT_FAILURE);
        }
        db->data = tmp;
    }
    memcpy(db->data + db->len, bytes, n);
    db->len += n;
}

/* Push a single byte into a dynamic buffer */
static void db_push1(DynBuf *db, uint8_t b)
{
    db_push(db, &b, 1);
}

/* Used for delta times in MIDI events */
static void db_push_vlq(DynBuf *db, uint32_t v)
{
    uint8_t tmp[4];
    int len = 0;
    tmp[len++] = v & 0x7F;
    v >>= 7;
    while (v)
    {
        tmp[len++] = (v & 0x7F) | 0x80;
        v >>= 7;
    }
    for (int i = len - 1; i >= 0; i--)
        db_push1(db, tmp[i]);
}

/* ==================== MIDI event emitters ==================== */

/* Sets the instrument */
static void emit_prog(DynBuf *db, uint8_t ch, uint8_t prog)
{
    db_push_vlq(db, 0);
    db_push1(db, 0xC0 | (ch & 0x0F));
    db_push1(db, prog);
}

/* Plays a note */
static void emit_on(DynBuf *db, uint32_t delta, uint8_t ch,
                    uint8_t note, uint8_t vel)
{
    db_push_vlq(db, delta);
    db_push1(db, 0x90 | (ch & 0x0F));
    db_push1(db, note);
    db_push1(db, vel);
}

/* Stops a note */
static void emit_off(DynBuf *db, uint32_t delta, uint8_t ch, uint8_t note)
{
    db_push_vlq(db, delta);
    db_push1(db, 0x80 | (ch & 0x0F));
    db_push1(db, note);
    db_push1(db, 0);
}

/* Wrap raw MIDI events inside a proper track chunk */
static uint8_t *make_mtrk(const uint8_t *events, size_t ev_len,
                          size_t *out_len)
{
    size_t total = 8 + ev_len + 4;
    uint8_t *chunk = malloc(total);
    if (!chunk)
    {
        perror("malloc mtrk");
        exit(EXIT_FAILURE);
    }

    chunk[0] = 'M';
    chunk[1] = 'T';
    chunk[2] = 'r';
    chunk[3] = 'k';
    uint32_t dlen = (uint32_t)(ev_len + 4);
    chunk[4] = (dlen >> 24) & 0xFF;
    chunk[5] = (dlen >> 16) & 0xFF;
    chunk[6] = (dlen >> 8) & 0xFF;
    chunk[7] = dlen & 0xFF;
    memcpy(chunk + 8, events, ev_len);
    chunk[8 + ev_len + 0] = 0x00;
    chunk[8 + ev_len + 1] = 0xFF;
    chunk[8 + ev_len + 2] = 0x2F;
    chunk[8 + ev_len + 3] = 0x00;

    *out_len = total;
    return chunk;
}

/* ==================== Music helpers ==================== */

static const char *genre_name(Genre genre)
{
    switch (genre)
    {
    case GENRE_POP:
        return "Pop";
    case GENRE_LOFI:
        return "Lo-fi";
    case GENRE_ROCK:
        return "Rock";
    case GENRE_LOONY_TUNES:
        return "Loony-Tunes";
    default:
        return "Unknown";
    }
}

/* Describes type of chord */
typedef enum
{
    CHORD_MAJOR = 0,
    CHORD_MINOR = 1,
    CHORD_DOM7 = 2,
    CHORD_MAJOR7 = 3,
    CHORD_MINOR7 = 4,
    CHORD_POWER5 = 5
} ChordQuality;

typedef struct
{
    const char *name;
    int roots[4];
    ChordQuality qualities[4];
} Progression;

#define PROG_COUNT(arr) ((int)(sizeof(arr) / sizeof((arr)[0])))

static const Progression *prog_get(const Progression *arr, int count, int idx)
{
    if (idx < 0 || idx >= count)
        idx = 0;
    return &arr[idx];
}

static const Progression LOFI_PROGRESSIONS[] = {
    {"Chill Jazzy Imaj7-vim7-iim7-V7",
     {0, 9, 2, 7},
     {CHORD_MAJOR7, CHORD_MINOR7, CHORD_MINOR7, CHORD_DOM7}},
    {"Chill Jazzy IVmaj7-iiim7-vim7-V7",
     {5, 4, 9, 7},
     {CHORD_MAJOR7, CHORD_MINOR7, CHORD_MINOR7, CHORD_DOM7}},
    {"Chill Jazzy bVIImaj7-vim7-iim7-vm7",
     {10, 9, 2, 7},
     {CHORD_MAJOR7, CHORD_MINOR7, CHORD_MINOR7, CHORD_MINOR7}},
    {"Dreamy Imaj7-V-vim7-IVmaj7",
     {0, 7, 9, 5},
     {CHORD_MAJOR7, CHORD_MAJOR, CHORD_MINOR7, CHORD_MAJOR7}},
    {"Dreamy Imaj7-iiim7-vim7-Vmaj7",
     {0, 4, 9, 7},
     {CHORD_MAJOR7, CHORD_MINOR7, CHORD_MINOR7, CHORD_MAJOR7}},
    {"Dreamy Imaj7-iiim7-vim7-IVmaj7",
     {0, 4, 9, 5},
     {CHORD_MAJOR7, CHORD_MINOR7, CHORD_MINOR7, CHORD_MAJOR7}},
    {"Sad vim7-IVmaj7-Imaj7-V",
     {9, 5, 0, 7},
     {CHORD_MINOR7, CHORD_MAJOR7, CHORD_MAJOR7, CHORD_MAJOR}},
    {"Neo-Soul Imaj7-III7-vim7-iim7",
     {0, 4, 9, 2},
     {CHORD_MAJOR7, CHORD_DOM7, CHORD_MINOR7, CHORD_MINOR7}},
    {"Neo-Soul IVmaj7-V7-iiim7-vim7",
     {5, 7, 4, 9},
     {CHORD_MAJOR7, CHORD_DOM7, CHORD_MINOR7, CHORD_MINOR7}},
    {"Neo-Soul bIIImaj7-iim7-vm7-im7",
     {3, 2, 7, 0},
     {CHORD_MAJOR7, CHORD_MINOR7, CHORD_MINOR7, CHORD_MINOR7}}};

static const Progression ROCK_PROGRESSIONS[] = {
    {"Classic Rock I-V-IV-I",
     {0, 7, 5, 0},
     {CHORD_MAJOR, CHORD_MAJOR, CHORD_MAJOR, CHORD_MAJOR}},
    {"Classic Rock I-IV-I-V",
     {0, 5, 0, 7},
     {CHORD_MAJOR, CHORD_MAJOR, CHORD_MAJOR, CHORD_MAJOR}},
    {"Classic Rock I-IV-V-IV",
     {0, 5, 7, 5},
     {CHORD_MAJOR, CHORD_MAJOR, CHORD_MAJOR, CHORD_MAJOR}},
    {"Classic Rock I-IV-V-I",
     {0, 5, 7, 0},
     {CHORD_MAJOR, CHORD_MAJOR, CHORD_MAJOR, CHORD_MAJOR}},
    {"Dark Rock im-bIII-bVII-IV",
     {0, 3, 10, 5},
     {CHORD_MINOR, CHORD_MAJOR, CHORD_MAJOR, CHORD_MAJOR}},
    {"Dark Rock im-bVII-bVI-V",
     {0, 10, 8, 7},
     {CHORD_MINOR, CHORD_MAJOR, CHORD_MAJOR, CHORD_MAJOR}},
    {"Dark Rock im-V-bVI-bVII",
     {0, 7, 8, 10},
     {CHORD_MINOR, CHORD_MAJOR, CHORD_MAJOR, CHORD_MAJOR}},
    {"Garage Punk I5-IV5-V5-IV5",
     {0, 5, 7, 5},
     {CHORD_POWER5, CHORD_POWER5, CHORD_POWER5, CHORD_POWER5}}};

static const Progression POP_PROGRESSIONS[] = {
    /* Classic pop */
    {"Classic I-V-vi-IV",
     {0, 7, 9, 5},
     {CHORD_MAJOR, CHORD_MAJOR, CHORD_MINOR, CHORD_MAJOR}},
    /* Bright / uplifting */
    {"Bright I-IV-vi-V",
     {0, 5, 9, 7},
     {CHORD_MAJOR, CHORD_MAJOR, CHORD_MINOR, CHORD_MAJOR}},
    {"Bright I-IV-V-vi",
     {0, 5, 7, 9},
     {CHORD_MAJOR, CHORD_MAJOR, CHORD_MAJOR, CHORD_MINOR}},
    /* Emotional */
    {"Emotional vi-IV-I-V",
     {9, 5, 0, 7},
     {CHORD_MINOR, CHORD_MAJOR, CHORD_MAJOR, CHORD_MAJOR}},
    {"Emotional vi-IV-I-III",
     {9, 5, 0, 4},
     {CHORD_MINOR, CHORD_MAJOR, CHORD_MAJOR, CHORD_MAJOR}},
    /* Softer / dreamy (with 7ths) */
    {"Dreamy Imaj7-V-vim7-IVmaj7",
     {0, 7, 9, 5},
     {CHORD_MAJOR7, CHORD_MAJOR, CHORD_MINOR7, CHORD_MAJOR7}},
    {"Dreamy Imaj7-IVmaj7-vim7-V",
     {0, 5, 9, 7},
     {CHORD_MAJOR7, CHORD_MAJOR7, CHORD_MINOR7, CHORD_MAJOR}},
};

static const Progression *get_progression(Genre genre, int lofi_prog, int rock_prog, int pop_prog)
{
    switch (genre)
    {
    case GENRE_POP:
        return prog_get(POP_PROGRESSIONS, PROG_COUNT(POP_PROGRESSIONS), pop_prog);
    case GENRE_LOFI:
        return prog_get(LOFI_PROGRESSIONS, PROG_COUNT(LOFI_PROGRESSIONS), lofi_prog);
    case GENRE_ROCK:
        return prog_get(ROCK_PROGRESSIONS, PROG_COUNT(ROCK_PROGRESSIONS), rock_prog);
    default:
        return NULL;
    }
}

static int bar_root(int key_root, int bar, Genre genre, int lofi_prog, int rock_prog, int pop_prog)
{
    static const int loony_tunes[] = {0, 5, 9, 7};
    const Progression *p = get_progression(genre, lofi_prog, rock_prog, pop_prog);
    if (p)
        return key_root + p->roots[bar % 4];
    if (genre == GENRE_LOONY_TUNES)
        return key_root + loony_tunes[bar % 4];
    return key_root;
}

static ChordQuality chord_quality(Genre genre, int bar, int lofi_prog, int rock_prog, int pop_prog)
{
    const Progression *p = get_progression(genre, lofi_prog, rock_prog, pop_prog);
    if (p)
        return p->qualities[bar % 4];
    if (genre == GENRE_LOONY_TUNES)
        return ((bar % 4) == 2) ? CHORD_MINOR : CHORD_MAJOR;
    return CHORD_MAJOR;
}

static int chord_is_minor(Genre genre, int bar, int lofi_prog, int rock_prog, int pop_prog)
{
    ChordQuality quality = chord_quality(genre, bar, lofi_prog, rock_prog, pop_prog);
    return quality == CHORD_MINOR || quality == CHORD_MINOR7;
}

/* Chord voicing: triad for most genres, seventh chords for lo-fi templates */
static int chord_notes(int root, ChordQuality quality, int out[4])
{
    int minor = (quality == CHORD_MINOR || quality == CHORD_MINOR7);

    out[0] = root;
    if (quality == CHORD_POWER5)
    {
        out[1] = root + 7;
        out[2] = root + 12;
        return 3;
    }
    out[1] = root + (minor ? 3 : 4);
    out[2] = root + 7;

    if (quality == CHORD_DOM7 || quality == CHORD_MINOR7)
    {
        out[3] = root + 10;
        return 4;
    }
    if (quality == CHORD_MAJOR7)
    {
        out[3] = root + 11;
        return 4;
    }
    return 3;
}

/* one MIDI event: used by all three generators */
typedef struct
{
    uint32_t tick; /* absolute tick position */
    int is_on;     /* 1 = note-on, 0 = note-off */
    uint8_t note;
    uint8_t vel;
} MidiEv;

/* sort by tick; note-offs before note-ons at the same tick to avoid stuck notes */
static int midi_ev_cmp(const void *a, const void *b)
{
    const MidiEv *ma = (const MidiEv *)a;
    const MidiEv *mb = (const MidiEv *)b;
    if (ma->tick != mb->tick)
        return (ma->tick < mb->tick) ? -1 : 1;
    return ma->is_on - mb->is_on;
}

/* sort, emit deltas into db, and free evs */
static void flush_events(DynBuf *db, MidiEv *evs, int nev, uint8_t ch)
{
    qsort(evs, (size_t)nev, sizeof(MidiEv), midi_ev_cmp);
    uint32_t cur = 0;
    for (int i = 0; i < nev; i++)
    {
        uint32_t delta = evs[i].tick - cur;
        cur = evs[i].tick;
        if (evs[i].is_on)
            emit_on(db, delta, ch, evs[i].note, evs[i].vel);
        else
            emit_off(db, delta, ch, evs[i].note);
    }
    free(evs);
}

/* ==================== Drums ==================== */
/* GM channel 9: 36=Bass Drum, 38=Snare, 42=Closed HH, 46=Open HH, 49=Crash */

typedef struct
{
    const char *name;
    uint8_t kick_mask;
    uint8_t snare_mask;
    uint8_t open_hat_mask;
    uint8_t kick_vel;
    uint8_t snare_vel;
    uint8_t hat_vel;
} RockGroove;

static void push_drum_hit(MidiEv *evs, int *nev, uint32_t tick, uint32_t len,
                          uint8_t note, uint8_t vel)
{
    if (*nev + 2 > MAX_DRUM_EVENTS)
        return;

    evs[(*nev)++] = (MidiEv){tick, 1, note, vel};
    evs[(*nev)++] = (MidiEv){tick + len, 0, note, 0};
}

static void gen_drums(const SongSpec *s, DynBuf *db)
{
    const uint8_t CH = CH_DRUMS;
    static const RockGroove rock_grooves[] = {
        {"Current", 0x51, 0x44, 0x00, 100, 95, 72},
        {"Move Along / AAR", 0x59, 0x44, 0x88, 104, 98, 78},
        {"Incubus-ish", 0x35, 0x44, 0x22, 96, 92, 68},
        {"Mr. Brightside", 0x33, 0x44, 0x80, 102, 100, 80}};
    const RockGroove *rock_groove = NULL;

    MidiEv *evs = malloc(MAX_DRUM_EVENTS * sizeof(MidiEv));
    if (!evs)
    {
        perror("malloc drum events");
        exit(EXIT_FAILURE);
    }
    int nev = 0;

    if (s->genre == GENRE_ROCK)
    {
        rock_groove = &rock_grooves[rand() % (int)(sizeof(rock_grooves) / sizeof(rock_grooves[0]))];
        printf("[Drums] Rock groove: %s\n", rock_groove->name);
        fflush(stdout);
    }

    for (int bar = 0; bar < s->bars; bar++)
    {
        if (s->genre == GENRE_ROCK && nev + 2 <= MAX_DRUM_EVENTS)
        {
            uint32_t bar_tick = (uint32_t)(bar * BEATS_PER_BAR) * BEAT;
            push_drum_hit(evs, &nev, bar_tick, EIGHT, 49, 95);
        }

        for (int beat = 0; beat < BEATS_PER_BAR; beat++)
        {
            uint32_t bt = (uint32_t)(bar * BEATS_PER_BAR + beat) * BEAT;

            /* kick */
            int kick = 0;
            uint8_t kick_vel = 90;
            switch (s->genre)
            {
            case GENRE_LOFI:
                kick = (beat == 0 || beat == 2);
                kick_vel = (beat == 0) ? 92 : 82;
                break;
            case GENRE_ROCK:
                kick = 0;
                break;
            case GENRE_LOONY_TUNES:
                kick = 1;
                kick_vel = 112;
                break;
            default:
                kick = (beat == 0 || beat == 2);
                kick_vel = 90;
                break;
            }
            if (kick && nev + 2 <= MAX_DRUM_EVENTS)
            {
                uint8_t kick_note = 36;
                if (s->genre == GENRE_LOFI && beat == 0)
                    kick_note = 35;
                push_drum_hit(evs, &nev, bt, EIGHT, kick_note, kick_vel);
                if (s->genre == GENRE_LOFI && beat == 0)
                    push_drum_hit(evs, &nev, bt, EIGHT, 36, 74);
            }

            /* snare */
            if (s->genre != GENRE_ROCK && (beat == 1 || beat == 3) && nev + 2 <= MAX_DRUM_EVENTS)
            {
                uint8_t snare_vel = 80;
                if (s->genre == GENRE_LOFI)
                    snare_vel = 62;
                if (s->genre == GENRE_LOONY_TUNES)
                    snare_vel = 90;
                push_drum_hit(evs, &nev, bt, EIGHT, 38, snare_vel);
            }

            /* hats: two 8th notes per beat */
            for (int e = 0; e < 2; e++)
            {
                uint32_t ht = bt + (uint32_t)e * EIGHT;
                uint8_t note = 42;
                uint8_t vel = 60;
                int slot = beat * 2 + e;

                switch (s->genre)
                {
                case GENRE_LOFI:
                    note = (e == 1 && beat == 3) ? 46 : 42;
                    vel = (e == 0) ? 42 : 52;
                    break;
                case GENRE_ROCK:
                    if (rock_groove != NULL)
                    {
                        if ((rock_groove->kick_mask & (1u << slot)) != 0)
                            push_drum_hit(evs, &nev, ht, EIGHT / 2, 36,
                                          (slot == 0) ? rock_groove->kick_vel
                                                      : (uint8_t)(rock_groove->kick_vel - 10));
                        if ((rock_groove->snare_mask & (1u << slot)) != 0)
                            push_drum_hit(evs, &nev, ht, EIGHT / 2, 38, rock_groove->snare_vel);
                        note = ((rock_groove->open_hat_mask & (1u << slot)) != 0) ? 46 : 42;
                        vel = (note == 46) ? (uint8_t)(rock_groove->hat_vel + 6) : rock_groove->hat_vel;
                    }
                    break;
                case GENRE_LOONY_TUNES:
                    note = (e == 1) ? 46 : 42;
                    vel = (e == 1) ? 74 : 46;
                    break;
                default:
                    note = 42;
                    vel = 60;
                    break;
                }
                if (nev + 2 <= MAX_DRUM_EVENTS)
                {
                    push_drum_hit(evs, &nev, ht, EIGHT / 2, note, vel);
                }
            }
        }
    }

    flush_events(db, evs, nev, CH);
}

/* ==================== Guitar ==================== */

static void gen_guitar(const SongSpec *s, DynBuf *db)
{
    const uint8_t CH = CH_GUITAR;
    uint8_t prog;
    switch (s->genre)
    {
    case GENRE_LOFI:
        prog = 26;
        break; /* Jazz Guitar */
    case GENRE_ROCK:
        prog = 29;
        break; /* Overdriven Guitar */
    case GENRE_LOONY_TUNES:
        prog = 28;
        break; /* Muted Guitar */
    default:
        prog = 27;
        break; /* Electric Guitar (clean) */
    }

    emit_prog(db, CH, prog);

    MidiEv *evs = malloc(MAX_GUITAR_EVENTS * sizeof(MidiEv));
    if (!evs)
    {
        perror("malloc guitar events");
        exit(EXIT_FAILURE);
    }
    int nev = 0;

    for (int bar = 0; bar < s->bars; bar++)
    {
        int root = bar_root(s->key_root, bar, s->genre, s->lofi_prog, s->rock_prog, s->pop_prog);
        ChordQuality quality = chord_quality(s->genre, bar, s->lofi_prog, s->rock_prog, s->pop_prog);
        int notes[4];
        int note_count = chord_notes(root, quality, notes);

        for (int beat = 0; beat < BEATS_PER_BAR; beat++)
        {
            uint32_t bt = (uint32_t)(bar * BEATS_PER_BAR + beat) * BEAT;
            uint32_t off = bt + (BEAT * 3) / 4;
            uint8_t vel = (beat == 0 || beat == 2) ? 85 : 60;
            int play = 1;
            uint32_t strum = 10;

            switch (s->genre)
            {
            case GENRE_LOFI:
                play = (beat == 0 || beat == 2);
                off = bt + (BEAT * 7) / 8;
                vel = (beat == 0) ? 62 : 52;
                strum = 14;
                break;
            case GENRE_ROCK:
                play = 1;
                off = bt + (BEAT * 7) / 8;
                vel = (beat == 0 || beat == 2) ? 112 : 90;
                strum = 6;
                break;
            case GENRE_LOONY_TUNES:
                play = 1;
                off = bt + BEAT / 3;
                vel = (beat == 0) ? 82 : 68;
                strum = 4;
                break;
            default:
                play = 1;
                off = bt + (BEAT * 3) / 4;
                vel = (beat == 0 || beat == 2) ? 85 : 60;
                strum = 10;
                break;
            }

            if (!play)
                continue;

            for (int n = 0; n < note_count && nev + 2 <= MAX_GUITAR_EVENTS; n++)
            {
                evs[nev++] = (MidiEv){bt + (uint32_t)n * strum, 1, (uint8_t)notes[n], vel};
                evs[nev++] = (MidiEv){off + (uint32_t)n * (strum / 2), 0, (uint8_t)notes[n], 0};
            }
        }
    }

    flush_events(db, evs, nev, CH);
}

/* ==================== Piano ==================== */

/* Simple deterministic hash for variety without stdlib rand */
static unsigned piano_hash(unsigned a, unsigned b)
{
    unsigned h = a * 2654435761u + b * 2246822519u;
    h ^= h >> 16;
    return h;
}

/*
 * Per-genre 8th-note rhythm patterns for one bar (8 slots).
 * Each value: 0 = rest, 1 = chord tone, 2 = scale passing tone.
 * Multiple patterns per genre to rotate through bars.
 */
static const int POP_PATTERNS[][8] = {
    {1, 2, 1, 0, 1, 2, 2, 0}, /* melody + rests */
    {1, 0, 2, 1, 0, 1, 2, 1}, /* syncopated */
    {1, 2, 0, 1, 2, 0, 1, 2}, /* breathing room */
    {1, 1, 2, 0, 1, 2, 0, 1}, /* front-loaded */
};

static const int LOFI_PATTERNS[][8] = {
    {1, 0, 2, 0, 1, 0, 2, 0}, /* sparse, airy */
    {1, 0, 0, 2, 1, 0, 0, 1}, /* very sparse */
    {0, 1, 0, 2, 0, 1, 0, 0}, /* off-beat feel */
    {1, 2, 0, 0, 1, 0, 2, 0}, /* dreamy */
};

static const int ROCK_PATTERNS[][8] = {
    {1, 1, 2, 1, 1, 2, 1, 0}, /* driving */
    {1, 2, 1, 2, 1, 0, 1, 1}, /* energetic */
    {1, 0, 1, 1, 2, 1, 0, 1}, /* punchy gaps */
    {1, 1, 0, 1, 1, 2, 2, 1}, /* heavy */
};

static const int ROCK_CONTOURS[][8] = {
    {0, 2, 1, 2, 0, 2, 1, 0}, /* chord punches */
    {0, 1, 2, 1, 0, 1, 3, 2}, /* climbing hook */
    {0, 0, 2, 0, 0, 1, 2, 0}, /* pedal tone */
    {2, 1, 0, 1, 2, 3, 1, 0}, /* brighter lead-in */
};

static const int LOONY_TUNES_PATTERNS[][8] = {
    {1, 0, 1, 0, 1, 0, 1, 0}, /* pulsing */
    {1, 1, 0, 1, 1, 0, 1, 0}, /* arpeggiated feel */
    {1, 0, 1, 1, 0, 1, 0, 1}, /* offbeat accents */
    {0, 1, 0, 1, 1, 0, 1, 1}, /* delayed entry */
};

static void gen_piano(const SongSpec *s, DynBuf *db)
{
    static const int major_scale[] = {0, 2, 4, 5, 7, 9, 11};
    static const int minor_penta[] = {0, 3, 5, 7, 10};
    static const int major_penta[] = {0, 2, 4, 7, 9};
    static const int mixolydian[] = {0, 2, 4, 5, 7, 9, 10};
    const int *scale;
    int scale_len;
    const int (*patterns)[8];
    uint8_t prog;

    const uint8_t CH = CH_PIANO;

    switch (s->genre)
    {
    case GENRE_LOFI:
        prog = 5; /* Electric Piano 2 */
        scale = major_scale; scale_len = 7;
        patterns = LOFI_PATTERNS;
        break;
    case GENRE_ROCK:
        prog = 0; /* Acoustic Grand Piano */
        scale = mixolydian; scale_len = 7;
        patterns = ROCK_PATTERNS;
        break;
    case GENRE_LOONY_TUNES:
        prog = 5; /* Electric Piano 2 */
        scale = major_penta; scale_len = 5;
        patterns = LOONY_TUNES_PATTERNS;
        break;
    default:
        prog = 0; /* Acoustic Grand Piano */
        scale = major_scale; scale_len = 7;
        patterns = POP_PATTERNS;
        break;
    }
    emit_prog(db, CH, prog);

    MidiEv *evs = malloc(MAX_PIANO_EVENTS * sizeof(MidiEv));
    if (!evs)
    {
        perror("malloc piano events");
        exit(EXIT_FAILURE);
    }
    int nev = 0;

    for (int bar = 0; bar < s->bars; bar++)
    {
        int root = bar_root(s->key_root, bar, s->genre, s->lofi_prog, s->rock_prog, s->pop_prog);
        ChordQuality quality = chord_quality(s->genre, bar, s->lofi_prog, s->rock_prog, s->pop_prog);
        int chord[4];
        int chord_len = chord_notes(root, quality, chord);

        const int *bar_scale = scale;
        int bar_scale_len = scale_len;

        if (chord_is_minor(s->genre, bar, s->lofi_prog, s->rock_prog, s->pop_prog))
        {
            bar_scale = minor_penta;
            bar_scale_len = 5;
        }

        /* pick a rhythm pattern based on bar number for variety */
        int pat_idx = (int)(piano_hash((unsigned)bar, (unsigned)s->key_root) % 4u);
        const int *pat = patterns[pat_idx];

        int rock_contour_idx = 0;
        const int *rock_contour = NULL;
        if (s->genre == GENRE_ROCK)
        {
            rock_contour_idx = (int)(piano_hash((unsigned)bar, (unsigned)(s->rock_prog + s->key_root)) % 4u);
            rock_contour = ROCK_CONTOURS[rock_contour_idx];
        }

        for (int e = 0; e < 8 && nev + 2 <= MAX_PIANO_EVENTS; e++)
        {
            if (pat[e] == 0)
                continue; /* rest */

            uint32_t on_tick = (uint32_t)(bar * BEATS_PER_BAR * BEAT) + (uint32_t)e * EIGHT;
            uint32_t off_tick = on_tick + (EIGHT * 3) / 4;
            uint8_t vel = (e % 2 == 0) ? 80 : 60;
            int octave = 0;

            if (s->genre == GENRE_LOFI)
            {
                off_tick = on_tick + (EIGHT * 7) / 8;
                vel = (e % 2 == 0) ? 44 : 36;
                octave = -12;
            }
            else if (s->genre == GENRE_ROCK)
            {
                off_tick = on_tick + (EIGHT * 2) / 3;
                vel = (e % 2 == 0) ? 60 : 46;
                octave = -12;
            }
            else if (s->genre == GENRE_LOONY_TUNES)
            {
                off_tick = on_tick + EIGHT / 2;
                vel = (e % 2 == 0) ? 92 : 72;
                octave = (e >= 4) ? 12 : 0;
            }

            uint8_t note;
            if (s->genre == GENRE_ROCK)
            {
                int contour = rock_contour[e];
                if (pat[e] == 1)
                {
                    int ci = contour % chord_len;
                    int base = chord[ci];

                    if (rock_contour_idx == 2)
                    {
                        if (ci == 0)
                            base -= 12;
                        else if (ci == 2)
                            base -= 5;
                    }
                    else if (rock_contour_idx == 3 && e >= 4)
                    {
                        base += 12;
                    }
                    note = (uint8_t)(base + octave);
                }
                else
                {
                    int si = contour % bar_scale_len;
                    int base = root + bar_scale[si];

                    if (rock_contour_idx == 2 && e < 4)
                        base -= 12;
                    if (rock_contour_idx == 3 && e >= 5)
                        base += 12;
                    note = (uint8_t)(base + octave);
                }
            }
            else if (pat[e] == 1)
            {
                /* chord tone: cycle through chord notes based on position */
                int ci = (int)(piano_hash((unsigned)bar, (unsigned)e) % (unsigned)chord_len);
                note = (uint8_t)(chord[ci] + octave);
            }
            else
            {
                /* passing tone: pick a scale degree based on bar+position */
                int si = (int)(piano_hash((unsigned)bar + (unsigned)e, (unsigned)s->bpm) % (unsigned)bar_scale_len);
                note = (uint8_t)(root + bar_scale[si] + octave);
            }

            evs[nev++] = (MidiEv){on_tick, 1, note, vel};
            evs[nev++] = (MidiEv){off_tick, 0, note, 0};
        }
    }

    flush_events(db, evs, nev, CH);
}

/* ==================== Child process ==================== */
static void child_main(int spec_fd, int track_fd, WorkerRole role)
{
    const char *name = (role == WORKER_DRUMS)    ? "Drums"
                       : (role == WORKER_GUITAR) ? "Guitar"
                                                 : "Piano";

    printf("[%s] Worker started (PID %d)\n", name, (int)getpid());
    fflush(stdout);

    srand((unsigned int)(time(NULL) ^ (unsigned int)getpid()));

    /* 1. Receive spec */
    SongSpec spec;
    safe_read_exact(spec_fd, &spec, sizeof(spec), name);
    if (close(spec_fd) < 0)
    {
        perror("child close spec_fd");
        exit(EXIT_FAILURE);
    }

    printf("[%s] Spec received: BPM=%d bars=%d key=%d genre=%s\n",
           name, spec.bpm, spec.bars, spec.key_root, genre_name(spec.genre));
    if (spec.genre == GENRE_POP)
        printf("[%s] Pop progression: %s\n", name, prog_get(POP_PROGRESSIONS, PROG_COUNT(POP_PROGRESSIONS), spec.pop_prog)->name);
    if (spec.genre == GENRE_LOFI)
        printf("[%s] Lo-fi progression: %s\n", name, prog_get(LOFI_PROGRESSIONS, PROG_COUNT(LOFI_PROGRESSIONS), spec.lofi_prog)->name);
    if (spec.genre == GENRE_ROCK)
        printf("[%s] Rock progression: %s\n", name, prog_get(ROCK_PROGRESSIONS, PROG_COUNT(ROCK_PROGRESSIONS), spec.rock_prog)->name);
    fflush(stdout);

    /* 2. Generate MIDI events */
    DynBuf events;
    db_init(&events);
    switch (role)
    {
    case WORKER_DRUMS:
        gen_drums(&spec, &events);
        break;
    case WORKER_GUITAR:
        gen_guitar(&spec, &events);
        break;
    case WORKER_PIANO:
        gen_piano(&spec, &events);
        break;
    }

    /* 3. Wrap in MTrk chunk */
    size_t track_len;
    uint8_t *track = make_mtrk(events.data, events.len, &track_len);
    free(events.data);

    printf("[%s] Generated %zu-byte MTrk chunk\n", name, track_len);
    fflush(stdout);

    /* 4. Send back: 4-byte length (native uint32) then raw bytes */
    uint32_t len32 = (uint32_t)track_len;
    safe_write(track_fd, &len32, sizeof(len32), name);
    safe_write(track_fd, track, track_len, name);
    free(track);

    if (close(track_fd) < 0)
    {
        perror("child close track_fd");
        exit(EXIT_FAILURE);
    }

    printf("[%s] Worker finished (PID %d)\n", name, (int)getpid());
    fflush(stdout);
    exit(EXIT_SUCCESS);
}

/* ==================== Write MIDI file ==================== */
static void write_midi_file(const char *path, int bpm,
                            const TrackBuf tracks[NUM_WORKERS])
{
    FILE *f = fopen(path, "wb");
    if (!f)
    {
        perror("fopen output.mid");
        exit(EXIT_FAILURE);
    }

    /* MThd: format=1, ntrks=4 (tempo + 3 instruments), division=480 */
    uint8_t mthd[14] = {
        'M', 'T', 'h', 'd',
        0, 0, 0, 6,        /* chunk length */
        0, 1,              /* format 1     */
        0, 4,              /* 4 tracks     */
        (TPQ >> 8) & 0xFF, /* division hi  */
        TPQ & 0xFF         /* division lo  */
    };
    if (fwrite(mthd, 1, sizeof(mthd), f) != sizeof(mthd))
    {
        perror("fwrite MThd");
        fclose(f);
        exit(EXIT_FAILURE);
    }

    /* Tempo track */
    uint32_t uspqn = 60000000u / (uint32_t)bpm;
    uint8_t tempo_events[] = {
        0x00,             /* delta = 0           */
        0xFF, 0x51, 0x03, /* meta: set tempo, 3b */
        (uint8_t)((uspqn >> 16) & 0xFF),
        (uint8_t)((uspqn >> 8) & 0xFF),
        (uint8_t)(uspqn & 0xFF),
        0x00, 0xFF, 0x2F, 0x00 /* end-of-track        */
    };
    uint32_t tdlen = (uint32_t)sizeof(tempo_events);
    uint8_t mtrk_hdr[8] = {
        'M', 'T', 'r', 'k',
        (uint8_t)((tdlen >> 24) & 0xFF),
        (uint8_t)((tdlen >> 16) & 0xFF),
        (uint8_t)((tdlen >> 8) & 0xFF),
        (uint8_t)(tdlen & 0xFF)};
    if (fwrite(mtrk_hdr, 1, 8, f) != 8 ||
        fwrite(tempo_events, 1, sizeof(tempo_events), f) != sizeof(tempo_events))
    {
        perror("fwrite tempo track");
        fclose(f);
        exit(EXIT_FAILURE);
    }

    /* Instrument tracks */
    for (int i = 0; i < NUM_WORKERS; i++)
    {
        if (fwrite(tracks[i].data, 1, tracks[i].len, f) != tracks[i].len)
        {
            perror("fwrite instrument track");
            fclose(f);
            exit(EXIT_FAILURE);
        }
    }
    if (fclose(f) != 0)
    {
        perror("fclose");
        exit(EXIT_FAILURE);
    }
}

static void try_play_with_timidity(const char *path)
{
    printf("[Parent] Launching TiMidity++ playback...\n");
    fflush(stdout);

    pid_t pid = fork();
    if (pid < 0)
    {
        perror("fork timidity");
        return;
    }

    if (pid == 0)
    {
        execlp("timidity", "timidity", path, (char *)NULL);
        perror("execlp timidity");
        _exit(127);
    }

    int status;
    if (waitpid(pid, &status, 0) < 0)
    {
        perror("waitpid timidity");
        return;
    }

    if (WIFEXITED(status) && WEXITSTATUS(status) == 127)
    {
        printf("[Parent] TiMidity++ is not installed.\n");
        printf("         Install it, then re-run this program:\n");
        printf("         brew install timidity\n");
        fflush(stdout);
        return;
    }

    if (WIFEXITED(status) && WEXITSTATUS(status) == 0)
    {
        printf("[Parent] TiMidity++ playback finished.\n");
    }
    else
    {
        printf("[Parent] TiMidity++ exited abnormally.\n");
    }
    fflush(stdout);
}

/* ==================== Input helpers ==================== */
static int read_int(const char *prompt, int lo, int hi)
{
    int v;
    while (1)
    {
        printf("%s [%d-%d]: ", prompt, lo, hi);
        fflush(stdout);
        if (scanf("%d", &v) == 1 && v >= lo && v <= hi)
            return v;
        printf("  Invalid – try again.\n");
        int c;
        while ((c = getchar()) != '\n' && c != EOF)
            ;
    }
}

/* Convert note name (e.g. "C4", "G#3", "Bb2") to MIDI number. Returns -1 if invalid. */
static int note_to_midi(const char *s)
{
    /* semitone offsets: C D E F G A B */
    static const int semitones[] = {0, 2, 4, 5, 7, 9, 11};
    static const char *names = "CDEFGAB";
    if (!s || !s[0])
        return -1;

    char letter = (char)(s[0] >= 'a' ? s[0] - 32 : s[0]);
    const char *pos = strchr(names, letter);
    if (!pos)
        return -1;

    int semi = semitones[pos - names];
    int i = 1;
    if (s[i] == '#')
    {
        semi++;
        i++;
    }
    else if (s[i] == 'b')
    {
        semi--;
        i++;
    }

    if (s[i] < '0' || s[i] > '9')
        return -1;
    int octave = s[i] - '0';
    int midi = (octave + 1) * 12 + semi;
    return (midi >= 0 && midi <= 127) ? midi : -1;
}

static int read_note(void)
{
    char buf[16];
    printf("Key root – pick any note from the list below:\n");
    printf("  C3  C#3  D3  D#3  E3  F3  F#3  G3  G#3  A3  A#3  B3\n");
    printf("  C4  C#4  D4  D#4  E4  F4  F#4  G4  G#4  A4  A#4  B4\n");
    printf("  C5  C#5  D5  D#5  E5  F5  F#5  G5  G#5  A5  A#5  B5\n");
    printf("  C6\n");
    printf("  (flats also work: Bb3, Eb4, Ab5 etc.)\n");
    while (1)
    {
        printf("Key root: ");
        fflush(stdout);
        if (scanf("%15s", buf) == 1)
        {
            int midi = note_to_midi(buf);
            if (midi >= 48 && midi <= 84)
                return midi;
        }
        printf("  Invalid – try a note like C4, G#3, Bb2 (C3 to C6).\n");
        int c;
        while ((c = getchar()) != '\n' && c != EOF)
            ;
    }
}

/* ==================== main ==================== */
int main(void)
{
    printf("╔══════════════════════════════════════╗\n");
    printf("║       MIDI Banger Generator          ║\n");
    printf("╚══════════════════════════════════════╝\n\n");

    /* ── Collect parameters ─────────────────────────────────────────────── */
    int bpm = read_int("BPM (tempo)", 40, 240);
    printf("\n");

    int bars = read_int("Number of bars", 1, 64);
    printf("\n");

    printf("Genre  0=Pop  1=Lo-fi  2=Rock  3=Loony-Tunes\n");
    Genre genre = (Genre)read_int("Genre", 0, 3);
    printf("\n");

    int key_root = read_note();
    printf("\n");
    int lofi_prog = 0;
    int rock_prog = 0;
    int pop_prog = 0;

    srand((unsigned int)(time(NULL) ^ (unsigned int)getpid()));
    if (genre == GENRE_POP)
        pop_prog = rand() % PROG_COUNT(POP_PROGRESSIONS);
    if (genre == GENRE_LOFI)
        lofi_prog = rand() % PROG_COUNT(LOFI_PROGRESSIONS);
    if (genre == GENRE_ROCK)
        rock_prog = rand() % PROG_COUNT(ROCK_PROGRESSIONS);

    printf("──────────────── LOG ────────────────\n");
    printf("[Parent] BPM=%d  bars=%d  genre=%s  key=%d\n",
           bpm, bars, genre_name(genre), key_root);
    if (genre == GENRE_POP)
        printf("[Parent] Pop progression: %s\n", prog_get(POP_PROGRESSIONS, PROG_COUNT(POP_PROGRESSIONS), pop_prog)->name);
    if (genre == GENRE_LOFI)
        printf("[Parent] Lo-fi progression: %s\n", prog_get(LOFI_PROGRESSIONS, PROG_COUNT(LOFI_PROGRESSIONS), lofi_prog)->name);
    if (genre == GENRE_ROCK)
        printf("[Parent] Rock progression: %s\n", prog_get(ROCK_PROGRESSIONS, PROG_COUNT(ROCK_PROGRESSIONS), rock_prog)->name);
    fflush(stdout);

    /* ── Create pipes ────────────────────────────────────────────────────── */
    int to_child[NUM_WORKERS][2];
    int from_child[NUM_WORKERS][2];

    for (int i = 0; i < NUM_WORKERS; i++)
    {
        if (pipe(to_child[i]) < 0)
        {
            perror("pipe to_child");
            exit(EXIT_FAILURE);
        }
        if (pipe(from_child[i]) < 0)
        {
            perror("pipe from_child");
            exit(EXIT_FAILURE);
        }
    }
    printf("[Parent] %d pipe pairs created\n", NUM_WORKERS * 2);
    fflush(stdout);

    /* ── Fork all three workers concurrently ─────────────────────────────── */
    pid_t pids[NUM_WORKERS];
    WorkerRole roles[NUM_WORKERS] = {WORKER_DRUMS, WORKER_GUITAR, WORKER_PIANO};

    for (int i = 0; i < NUM_WORKERS; i++)
    {
        pid_t pid = fork();
        if (pid < 0)
        {
            perror("fork");
            exit(EXIT_FAILURE);
        }

        if (pid == 0)
        {
            /* ── Child: close every pipe end except its own two ── */
            for (int j = 0; j < NUM_WORKERS; j++)
            {
                /* Parent side of to_child: child never writes */
                if (close(to_child[j][1]) < 0)
                {
                    perror("child close");
                    exit(EXIT_FAILURE);
                }
                /* Parent side of from_child: child never reads */
                if (close(from_child[j][0]) < 0)
                {
                    perror("child close");
                    exit(EXIT_FAILURE);
                }
                /* Sibling pipes: not our business */
                if (j != i)
                {
                    if (close(to_child[j][0]) < 0)
                    {
                        perror("child close");
                        exit(EXIT_FAILURE);
                    }
                    if (close(from_child[j][1]) < 0)
                    {
                        perror("child close");
                        exit(EXIT_FAILURE);
                    }
                }
            }
            /* Run and never return */
            child_main(to_child[i][0], from_child[i][1], roles[i]);
        }

        pids[i] = pid;
        printf("[Parent] Forked %s worker (PID %d)\n",
               (roles[i] == WORKER_DRUMS)    ? "Drums"
               : (roles[i] == WORKER_GUITAR) ? "Guitar"
                                             : "Piano",
               pid);
        fflush(stdout);
    }

    /* ── Parent: close child-side pipe ends ──────────────────────────────── */
    for (int i = 0; i < NUM_WORKERS; i++)
    {
        if (close(to_child[i][0]) < 0)
        {
            perror("parent close to_child[0]");
            exit(EXIT_FAILURE);
        }
        if (close(from_child[i][1]) < 0)
        {
            perror("parent close from_child[1]");
            exit(EXIT_FAILURE);
        }
    }

    /* ── Send spec to each worker ─────────────────────────────────────────── */
    printf("\n[Parent] Sending specs to all workers...\n");
    fflush(stdout);
    for (int i = 0; i < NUM_WORKERS; i++)
    {
        SongSpec spec = {
            .bpm = bpm,
            .bars = bars,
            .key_root = key_root,
            .genre = genre,
            .lofi_prog = lofi_prog,
            .rock_prog = rock_prog,
            .pop_prog = pop_prog,
            .role = roles[i]};
        safe_write(to_child[i][1], &spec, sizeof(spec), "parent→child spec");
        if (close(to_child[i][1]) < 0)
        {
            perror("parent close to_child[1]");
            exit(EXIT_FAILURE);
        }
        printf("[Parent] Spec sent to worker %d\n", i);
        fflush(stdout);
    }

    /* ── Receive track data from each worker ─────────────────────────────── */
    printf("\n[Parent] Collecting MIDI tracks from workers...\n");
    fflush(stdout);

    TrackBuf tracks[NUM_WORKERS];
    memset(tracks, 0, sizeof(tracks));

    for (int i = 0; i < NUM_WORKERS; i++)
    {
        uint32_t len32;
        safe_read_exact(from_child[i][0], &len32, sizeof(len32), "parent←child len");

        tracks[i].data = malloc(len32);
        if (!tracks[i].data)
        {
            perror("malloc tracks[i]");
            exit(EXIT_FAILURE);
        }
        tracks[i].len = len32;
        safe_read_exact(from_child[i][0], tracks[i].data, len32, "parent←child data");

        if (close(from_child[i][0]) < 0)
        {
            perror("parent close from_child[0]");
            exit(EXIT_FAILURE);
        }
        printf("[Parent] Received %u bytes from %s worker\n",
               len32,
               (roles[i] == WORKER_DRUMS)    ? "Drums"
               : (roles[i] == WORKER_GUITAR) ? "Guitar"
                                             : "Piano");
        fflush(stdout);
    }

    /* ── Wait for all workers (prevent zombies) ───────────────────────────── */
    printf("\n[Parent] Waiting for worker processes to exit...\n");
    fflush(stdout);
    for (int i = 0; i < NUM_WORKERS; i++)
    {
        int status;
        if (waitpid(pids[i], &status, 0) < 0)
        {
            perror("waitpid");
            exit(EXIT_FAILURE);
        }
        if (WIFEXITED(status))
            printf("[Parent] Worker PID %d exited with code %d\n",
                   pids[i], WEXITSTATUS(status));
        else
            printf("[Parent] Worker PID %d terminated abnormally\n", pids[i]);
        fflush(stdout);
    }

    /* ── Merge and write MIDI file ────────────────────────────────────────── */
    printf("\n[Parent] Merging tracks → %s ...\n", OUTPUT_FILE);
    fflush(stdout);
    write_midi_file(OUTPUT_FILE, bpm, tracks);

    /* ── Free all heap memory ────────────────────────────────────────────── */
    for (int i = 0; i < NUM_WORKERS; i++)
    {
        free(tracks[i].data);
        tracks[i].data = NULL;
    }

    try_play_with_timidity(OUTPUT_FILE);

    printf("─────────────────────────────────────\n");
    printf("\n");
    printf("╔══════════════════════════════════════╗\n");
    printf("║         Banger Generated!            ║\n");
    printf("║   output.mid                         ║\n");
    printf("╚══════════════════════════════════════╝\n\n");
    fflush(stdout);
    return EXIT_SUCCESS;
}
