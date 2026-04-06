# MIDI Banger Generator

A C program that generates randomised backing tracks using concurrent child processes and raw MIDI file construction — no external music libraries required.

---

## How It Works

The parent process reads user parameters (BPM, genre, key, bars), then spawns **3 child processes** concurrently over pipes — one for each instrument. Each child independently generates a MIDI track and pipes it back to the parent, which merges them into a single `.mid` file and renders it to `.ogg` via TiMidity++.

```
                        ┌─────────────────────────┐
                        │       PARENT PROCESS      │
                        │                           │
                        │  Reads: BPM, Genre,       │
                        │         Key, Bars         │
                        └────────────┬──────────────┘
                                     │
                    fork() × 3  +  pipes
                                     │
          ┌──────────────────────────┼──────────────────────────┐
          │                          │                          │
          ▼                          ▼                          ▼
 ┌────────────────┐        ┌────────────────┐        ┌────────────────┐
 │  CHILD: DRUMS  │        │ CHILD: GUITAR  │        │  CHILD: PIANO  │
 │                │        │                │        │                │
 │ Kick, snare,   │        │ Strummed chord │        │ Melodic runs   │
 │ hi-hat pattern │        │ progressions   │        │ chord voicing  │
 └───────┬────────┘        └───────┬────────┘        └───────┬────────┘
         │                         │                          │
         │    MIDI track bytes (via pipe)                     │
         └─────────────────────────┼──────────────────────────┘
                                   │
                                   ▼
                        ┌─────────────────────────┐
                        │       PARENT PROCESS      │
                        │                           │
                        │  Merges 3 tracks into     │
                        │  SMF Type 1 MIDI file     │
                        │                           │
                        │  Calls TiMidity++ to      │
                        │  render → output.ogg      │
                        └─────────────────────────┘
```

---

## Genres

| # | Genre | Chord Style | Progressions |
|---|-------|-------------|--------------|
| 0 | Pop | Triads + 7ths | 7 (Classic, Bright, Emotional, Dreamy) |
| 1 | Lo-fi | 7th chords (jazzy) | 10 (Chill, Dreamy, Sad, Neo-Soul) |
| 2 | Rock | Power chords + triads | 8 (Classic, Dark, Garage Punk) |
| 3 | Loony Tunes | Major/minor triads | 1 (fixed cartoon progression) |

Each genre has custom drum patterns, guitar voicings, piano scales, and instrument patches (General MIDI).

---

## Requirements

- GCC (C99)
- TiMidity++ — for rendering MIDI to audio

```bash
sudo apt install timidity        # Debian/Ubuntu
brew install timidity            # macOS (Homebrew)
```

---

## Build & Run

```bash
make
./midi_gen
```

The program is fully interactive. You will be prompted for:

```
BPM (tempo) [40-240]:
Number of bars [1-64]:
Genre  0=Pop  1=Lo-fi  2=Rock  3=Loony-Tunes
Key root (e.g. C4, G#3, Bb4 — full list shown at prompt)
```

---

## Output

| File | Description |
|------|-------------|
| `output.mid` | Raw MIDI file (SMF Type 1, 4 tracks) |
| `output.ogg` | Rendered audio — open in any media player |

---

## Project Structure

```
midi_gen.c      — full source (parent + 3 child workers)
Makefile        — builds with: make
```

---

## Example Session

```
╔══════════════════════════════════════╗
║       MIDI Banger Generator          ║
╚══════════════════════════════════════╝

BPM (tempo) [40-240]: 120

Number of bars [1-64]: 8

Genre  0=Pop  1=Lo-fi  2=Rock  3=Loony-Tunes
Genre [0-3]: 2

Key root: G3

──────────────── LOG ────────────────
[Parent] BPM=120  bars=8  genre=Rock  key=55
[Parent] Rock progression: Dark Rock im-bVII-bVI-V
[Drums]  Genre selected: Rock
[Guitar] Genre selected: Rock
[Piano]  Genre selected: Rock
─────────────────────────────────────

╔══════════════════════════════════════╗
║         Banger Generated!            ║
║                                      ║
║   MIDI  ->  output.mid               ║
║   Audio ->  output.ogg               ║
╚══════════════════════════════════════╝
```
