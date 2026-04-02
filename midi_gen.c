/*
 * midi_gen.c  –  MIDI Song Generator using pipes & concurrent child processes
 *
 * Architecture:
 *   Parent (controller) reads user parameters, spawns 3 worker processes
 *   concurrently (drums, guitar, piano), sends each a spec over a pipe,
 *   collects the generated MIDI track data back via pipes, then merges
 *   all tracks into a single Standard MIDI File (SMF format 1).
 *
 * Pipe layout (per worker i):
 *   to_child[i][0/1]   – parent writes spec  → child reads spec
 *   from_child[i][0/1] – child writes track   → parent reads track
 *
 * MIDI output: output.mid  (SMF Type 1, 4 tracks: tempo + drums/guitar/piano)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <stdint.h>
#include <errno.h>

/* ==================== Constants ==================== */

#define NUM_WORKERS 3
#define OUTPUT_FILE "output.mid"
#define TPQ 480 /* ticks per quarter note */

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
    GENRE_EDM = 3
} Genre;

/* read by child as raw bytes over pipe */
typedef struct
{
    int bpm;
    int bars;
    int beats_per_bar;
    int key_root; /* MIDI root note, e.g. C4 = 60 */
    int genre; /* 0=pop, 1=lofi, 2=rock, 3=edm */
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

static void db_push1(DynBuf *db, uint8_t b)
{
    db_push(db, &b, 1);
}

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

static void emit_prog(DynBuf *db, uint8_t ch, uint8_t prog)
{
    db_push_vlq(db, 0);
    db_push1(db, 0xC0 | (ch & 0x0F));
    db_push1(db, prog);
}

static void emit_on(DynBuf *db, uint32_t delta, uint8_t ch,
                    uint8_t note, uint8_t vel)
{
    db_push_vlq(db, delta);
    db_push1(db, 0x90 | (ch & 0x0F));
    db_push1(db, note);
    db_push1(db, vel);
}

static void emit_off(DynBuf *db, uint32_t delta, uint8_t ch, uint8_t note)
{
    db_push_vlq(db, delta);
    db_push1(db, 0x80 | (ch & 0x0F));
    db_push1(db, note);
    db_push1(db, 0);
}

/* Wrap raw event bytes inside a proper MTrk chunk (heap-allocated) */
static uint8_t *make_mtrk(const uint8_t *events, size_t ev_len,
                          size_t *out_len)
{
    /* MTrk header (8) + events + end-of-track meta (4) */
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
    uint32_t dlen = (uint32_t)(ev_len + 4); /* include end-of-track */
    chunk[4] = (dlen >> 24) & 0xFF;
    chunk[5] = (dlen >> 16) & 0xFF;
    chunk[6] = (dlen >> 8) & 0xFF;
    chunk[7] = dlen & 0xFF;
    memcpy(chunk + 8, events, ev_len);
    /* end-of-track: delta=0, FF 2F 00 */
    chunk[8 + ev_len + 0] = 0x00;
    chunk[8 + ev_len + 1] = 0xFF;
    chunk[8 + ev_len + 2] = 0x2F;
    chunk[8 + ev_len + 3] = 0x00;

    *out_len = total;
    return chunk;
}

/* ═══════════════════════════════════════════════════════════════════════════ */
/*  Music helpers                                                               */
/* ═══════════════════════════════════════════════════════════════════════════ */

static const char *genre_name(int genre)
{
    switch (genre)
    {
    case GENRE_POP:
        return "Pop";
    case GENRE_LOFI:
        return "Lo-fi";
    case GENRE_ROCK:
        return "Rock";
    case GENRE_EDM:
        return "EDM";
    default:
        return "Unknown";
    }
}

/* Four-bar progressions per genre (semitones from root). */
static int bar_root(int key_root, int bar, int genre)
{
    static const int pop[] = {0, 7, 9, 5};
    static const int lofi[] = {0, 9, 5, 7};
    static const int rock[] = {0, 5, 7, 0};
    static const int edm[] = {0, 5, 9, 7};
    const int *prog = pop;

    switch (genre)
    {
    case GENRE_LOFI:
        prog = lofi;
        break;
    case GENRE_ROCK:
        prog = rock;
        break;
    case GENRE_EDM:
        prog = edm;
        break;
    default:
        prog = pop;
        break;
    }

    return key_root + prog[bar % 4];
}

static int chord_is_minor(int genre, int bar)
{
    switch (genre)
    {
    case GENRE_POP:
        return (bar % 4) == 2; /* vi */
    case GENRE_LOFI:
        return (bar % 4) == 1; /* vi */
    case GENRE_ROCK:
        return 0;
    case GENRE_EDM:
        return (bar % 4) == 2; /* vi */
    default:
        return 0;
    }
}

/* Chord voicing: root + (minor? 3 : 4) + 7 */
static void chord_notes(int root, int minor, int out[3])
{
    out[0] = root;
    out[1] = root + (minor ? 3 : 4);
    out[2] = root + 7;
}

/* ═══════════════════════════════════════════════════════════════════════════ */
/*  Generator: DRUMS                                                            */
/* ═══════════════════════════════════════════════════════════════════════════ */
/*
 * GM drum note map (channel 9):
 *   36 = Bass Drum, 38 = Snare, 42 = Closed HH, 46 = Open HH, 49 = Crash
 *
 * Patterns:
 *   Pop  – kick 1&3, snare 2&4, closed HH every 8th note
 *   Lo-fi– softer backbeat with airy off-beat hats
 *   Rock – punchier kick/snare with a crash at bar starts
 *   EDM  – four-on-the-floor kick, clap/snare 2&4, open hats on off-beats
 *
 * Strategy: collect (abs_tick, type, note, vel) events into an array sorted
 * by time, then emit with proper deltas.  This avoids unsigned-subtraction
 * wrapping when multiple voices land on the same beat.
 */

#define MAX_DRUM_EVENTS 8192

typedef struct
{
    uint32_t tick;
    int is_on;
    uint8_t note;
    uint8_t vel;
} DrumEv;

static int drum_ev_cmp(const void *a, const void *b)
{
    const DrumEv *da = (const DrumEv *)a;
    const DrumEv *db = (const DrumEv *)b;
    if (da->tick != db->tick)
        return (da->tick < db->tick) ? -1 : 1;
    /* note-offs before note-ons at same tick to avoid stuck notes */
    return (da->is_on - db->is_on);
}

static void gen_drums(const SongSpec *s, DynBuf *db)
{
    const uint32_t BEAT = TPQ;
    const uint32_t EIGHT = TPQ / 2;
    const uint8_t CH = CH_DRUMS;

    DrumEv *evs = malloc(MAX_DRUM_EVENTS * sizeof(DrumEv));
    if (!evs)
    {
        perror("malloc drum events");
        exit(EXIT_FAILURE);
    }
    int nev = 0;

    for (int bar = 0; bar < s->bars; bar++)
    {
        if (s->genre == GENRE_ROCK && nev + 2 <= MAX_DRUM_EVENTS)
        {
            uint32_t bar_tick = (uint32_t)(bar * s->beats_per_bar) * BEAT;
            evs[nev++] = (DrumEv){bar_tick, 1, 49, 95};
            evs[nev++] = (DrumEv){bar_tick + EIGHT, 0, 49, 0};
        }

        for (int beat = 0; beat < s->beats_per_bar; beat++)
        {
            uint32_t bt = (uint32_t)(bar * s->beats_per_bar + beat) * BEAT;

            /* ── Kick ── */
            int kick = 0;
            uint8_t kick_vel = 90;
            switch (s->genre)
            {
            case GENRE_LOFI:
                kick = (beat == 0 || beat == 2);
                kick_vel = 72;
                break;
            case GENRE_ROCK:
                kick = (beat == 0 || beat == 2 || beat == 3);
                kick_vel = (beat == 0) ? 100 : 88;
                break;
            case GENRE_EDM:
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
                evs[nev++] = (DrumEv){bt, 1, 36, kick_vel};
                evs[nev++] = (DrumEv){bt + EIGHT, 0, 36, 0};
            }

            /* ── Snare ── */
            if ((beat == 1 || beat == 3) && nev + 2 <= MAX_DRUM_EVENTS)
            {
                uint8_t snare_vel = 80;
                if (s->genre == GENRE_LOFI)
                    snare_vel = 62;
                if (s->genre == GENRE_ROCK)
                    snare_vel = 95;
                if (s->genre == GENRE_EDM)
                    snare_vel = 90;
                evs[nev++] = (DrumEv){bt, 1, 38, snare_vel};
                evs[nev++] = (DrumEv){bt + EIGHT, 0, 38, 0};
            }

            /* ── Hats: two 8th notes per beat ── */
            for (int e = 0; e < 2; e++)
            {
                uint32_t ht = bt + (uint32_t)e * EIGHT;
                uint8_t note = 42;
                uint8_t vel = 60;

                switch (s->genre)
                {
                case GENRE_LOFI:
                    note = (e == 1 && beat == 3) ? 46 : 42;
                    vel = (e == 0) ? 42 : 52;
                    break;
                case GENRE_ROCK:
                    note = 42;
                    vel = 72;
                    break;
                case GENRE_EDM:
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
                    evs[nev++] = (DrumEv){ht, 1, note, vel};
                    evs[nev++] = (DrumEv){ht + EIGHT / 2, 0, note, 0};
                }
            }
        }
    }

    qsort(evs, (size_t)nev, sizeof(DrumEv), drum_ev_cmp);

    uint32_t cur = 0;
    for (int i = 0; i < nev; i++)
    {
        uint32_t delta = evs[i].tick - cur;
        cur = evs[i].tick;
        if (evs[i].is_on)
            emit_on(db, delta, CH, evs[i].note, evs[i].vel);
        else
            emit_off(db, delta, CH, evs[i].note);
    }

    free(evs);
}

/* ═══════════════════════════════════════════════════════════════════════════ */
/*  Generator: GUITAR (block chord strums)                                     */
/* ═══════════════════════════════════════════════════════════════════════════ */

#define MAX_GUITAR_EVENTS 8192

typedef struct
{
    uint32_t tick;
    int is_on;
    uint8_t note;
    uint8_t vel;
} GuitarEv;

static int guitar_ev_cmp(const void *a, const void *b)
{
    const GuitarEv *ga = (const GuitarEv *)a;
    const GuitarEv *gb = (const GuitarEv *)b;
    if (ga->tick != gb->tick)
        return (ga->tick < gb->tick) ? -1 : 1;
    return ga->is_on - gb->is_on; /* note-offs first at same tick */
}

static void gen_guitar(const SongSpec *s, DynBuf *db)
{
    const uint32_t BEAT = TPQ;
    const uint8_t CH = CH_GUITAR;
    uint8_t prog;
    switch (s->genre) {
    case GENRE_LOFI: prog = 26; break; /* Jazz Guitar */
    case GENRE_ROCK: prog = 29; break; /* Overdriven Guitar */
    case GENRE_EDM:  prog = 28; break; /* Muted Guitar */
    default:         prog = 27; break; /* Electric Guitar (clean) */
    }

    emit_prog(db, CH, prog);

    GuitarEv *evs = malloc(MAX_GUITAR_EVENTS * sizeof(GuitarEv));
    if (!evs)
    {
        perror("malloc guitar events");
        exit(EXIT_FAILURE);
    }
    int nev = 0;

    for (int bar = 0; bar < s->bars; bar++)
    {
        int root = bar_root(s->key_root, bar, s->genre);
        int minor = chord_is_minor(s->genre, bar);
        int notes[3];
        chord_notes(root, minor, notes);

        for (int beat = 0; beat < s->beats_per_bar; beat++)
        {
            uint32_t bt = (uint32_t)(bar * s->beats_per_bar + beat) * BEAT;
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
                vel = (beat == 0 || beat == 2) ? 98 : 76;
                strum = 6;
                break;
            case GENRE_EDM:
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

            for (int n = 0; n < 3 && nev + 2 <= MAX_GUITAR_EVENTS; n++)
            {
                evs[nev++] = (GuitarEv){bt + (uint32_t)n * strum, 1, (uint8_t)notes[n], vel};
                evs[nev++] = (GuitarEv){off + (uint32_t)n * (strum / 2), 0, (uint8_t)notes[n], 0};
            }
        }
    }

    qsort(evs, (size_t)nev, sizeof(GuitarEv), guitar_ev_cmp);

    uint32_t cur = 0;
    for (int i = 0; i < nev; i++)
    {
        uint32_t delta = evs[i].tick - cur;
        cur = evs[i].tick;
        if (evs[i].is_on)
            emit_on(db, delta, CH, evs[i].note, evs[i].vel);
        else
            emit_off(db, delta, CH, evs[i].note);
    }

    free(evs);
}

/* ═══════════════════════════════════════════════════════════════════════════ */
/*  Generator: PIANO (8th-note walking melody over the scale)                  */
/* ═══════════════════════════════════════════════════════════════════════════ */

#define MAX_PIANO_EVENTS 4096

typedef struct
{
    uint32_t tick;
    int is_on;
    uint8_t note;
    uint8_t vel;
} PianoEv;

static int piano_ev_cmp(const void *a, const void *b)
{
    const PianoEv *pa = (const PianoEv *)a;
    const PianoEv *pb = (const PianoEv *)b;
    if (pa->tick != pb->tick)
        return (pa->tick < pb->tick) ? -1 : 1;
    return pa->is_on - pb->is_on;
}

static void gen_piano(const SongSpec *s, DynBuf *db)
{
    static const int major_scale[] = {0, 2, 4, 5, 7, 9, 11};
    static const int minor_penta[] = {0, 3, 5, 7, 10};
    static const int major_penta[] = {0, 2, 4, 7, 9};
    static const int mixolydian[] = {0, 2, 4, 5, 7, 9, 10};
    const int *scale = major_scale;
    int scale_len = 7;

    const uint32_t EIGHT = TPQ / 2;
    const uint8_t CH = CH_PIANO;
    int si = 0, dir = 1;

    uint8_t prog;
    switch (s->genre) {
    case GENRE_LOFI: prog = 4; break; /* Electric Piano 1 */
    case GENRE_ROCK: prog = 1; break; /* Bright Acoustic Piano */
    case GENRE_EDM:  prog = 5; break; /* Electric Piano 2 */
    default:         prog = 0; break; /* Acoustic Grand Piano */
    }
    emit_prog(db, CH, prog);

    switch (s->genre)
    {
    case GENRE_LOFI:
        scale = minor_penta;
        scale_len = 5;
        break;
    case GENRE_ROCK:
        scale = mixolydian;
        scale_len = 7;
        break;
    case GENRE_EDM:
        scale = major_penta;
        scale_len = 5;
        break;
    default:
        scale = major_scale;
        scale_len = 7;
        break;
    }

    PianoEv *evs = malloc(MAX_PIANO_EVENTS * sizeof(PianoEv));
    if (!evs)
    {
        perror("malloc piano events");
        exit(EXIT_FAILURE);
    }
    int nev = 0;

    for (int bar = 0; bar < s->bars; bar++)
    {
        int root = bar_root(s->key_root, bar, s->genre);

        for (int e = 0; e < 8 && nev + 2 <= MAX_PIANO_EVENTS; e++)
        {
            uint32_t on_tick = (uint32_t)(bar * s->beats_per_bar * TPQ) + (uint32_t)e * EIGHT;
            uint32_t off_tick = on_tick + (EIGHT * 3) / 4;
            uint8_t vel = (e % 2 == 0) ? 80 : 60;
            int octave = 0;

            if (s->genre == GENRE_LOFI)
            {
                off_tick = on_tick + (EIGHT * 7) / 8;
                vel = (e % 2 == 0) ? 56 : 46;
            }
            else if (s->genre == GENRE_ROCK)
            {
                off_tick = on_tick + (EIGHT * 2) / 3;
                vel = (e % 2 == 0) ? 88 : 70;
            }
            else if (s->genre == GENRE_EDM)
            {
                off_tick = on_tick + EIGHT / 2;
                vel = (e % 2 == 0) ? 92 : 72;
                octave = (e >= 4) ? 12 : 0;
            }

            uint8_t note = (uint8_t)(root + scale[si] + octave);

            evs[nev++] = (PianoEv){on_tick, 1, note, vel};
            evs[nev++] = (PianoEv){off_tick, 0, note, 0};

            si += dir;
            if (si >= scale_len)
            {
                si = scale_len - 2;
                dir = -1;
            }
            if (si < 0)
            {
                si = 1;
                dir = 1;
            }
        }
    }

    qsort(evs, (size_t)nev, sizeof(PianoEv), piano_ev_cmp);

    uint32_t cur = 0;
    for (int i = 0; i < nev; i++)
    {
        uint32_t delta = evs[i].tick - cur;
        cur = evs[i].tick;
        if (evs[i].is_on)
            emit_on(db, delta, CH, evs[i].note, evs[i].vel);
        else
            emit_off(db, delta, CH, evs[i].note);
    }

    free(evs);
}

/* ═══════════════════════════════════════════════════════════════════════════ */
/*  Child process entry point                                                   */
/* ═══════════════════════════════════════════════════════════════════════════ */
static void child_main(int spec_fd, int track_fd, WorkerRole role)
{
    const char *name = (role == WORKER_DRUMS)    ? "Drums"
                       : (role == WORKER_GUITAR) ? "Guitar"
                                                 : "Piano";

    printf("[%s] Worker started (PID %d)\n", name, (int)getpid());
    fflush(stdout);

    /* 1. Receive spec */
    SongSpec spec;
    safe_read_exact(spec_fd, &spec, sizeof(spec), name);
    if (close(spec_fd) < 0)
    {
        perror("child close spec_fd");
        exit(EXIT_FAILURE);
    }

    printf("[%s] Spec received: BPM=%d bars=%d key=%d genre=%d\n",
           name, spec.bpm, spec.bars, spec.key_root, spec.genre);
    printf("[%s] Genre selected: %s\n", name, genre_name(spec.genre));
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

/* ═══════════════════════════════════════════════════════════════════════════ */
/*  Write the final SMF Type-1 file                                             */
/* ═══════════════════════════════════════════════════════════════════════════ */
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

/* ═══════════════════════════════════════════════════════════════════════════ */
/*  Input helper                                                                */
/* ═══════════════════════════════════════════════════════════════════════════ */
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

/* ═══════════════════════════════════════════════════════════════════════════ */
/*  main                                                                        */
/* ═══════════════════════════════════════════════════════════════════════════ */
int main(void)
{
    printf("╔══════════════════════════════════════╗\n");
    printf("║      MIDI Song Generator  v1.0       ║\n");
    printf("╚══════════════════════════════════════╝\n\n");

    /* ── Collect parameters ─────────────────────────────────────────────── */
    int bpm = read_int("BPM (tempo)", 40, 240);
    int bars = read_int("Number of bars", 1, 64);
    printf("Genre  0=Pop  1=Lo-fi  2=Rock  3=EDM\n");
    int genre = read_int("Genre", 0, 3);
    printf("Key root MIDI note  (C4=60  D4=62  E4=64  F4=65  G4=67  A4=69  B4=71)\n");
    int key_root = read_int("Key root", 48, 84);

    printf("\n[Parent] Parameters: BPM=%d  bars=%d  genre=%s (%d)  key_root=%d\n\n",
           bpm, bars, genre_name(genre), genre, key_root);
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
            .beats_per_bar = 4,
            .key_root = key_root,
            .genre = genre,
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

    printf("[Parent] Done!  '%s' written to disk.\n", OUTPUT_FILE);
    try_play_with_timidity(OUTPUT_FILE);
    fflush(stdout);
    return EXIT_SUCCESS;
}
