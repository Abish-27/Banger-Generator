#include <stdio.h>
#include <stdint.h>

// General MIDI Instrument Numbers
#define AC_GUITAR 24 
#define TICKS_PER_BEAT 480

// G Major Chord Notes (G3, B3, D4, G4)
uint8_t g_major[] = {55, 59, 62, 67};

static void write_u16(FILE *f, uint16_t v) {
    fputc((v >> 8) & 0xFF, f);
    fputc(v & 0xFF, f);
}

static void write_u32(FILE *f, uint32_t v) {
    fputc((v >> 24) & 0xFF, f);
    fputc((v >> 16) & 0xFF, f);
    fputc((v >>  8) & 0xFF, f);
    fputc( v       & 0xFF, f);
}

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

int main(void) {
    FILE *f = fopen("guitar.mid", "wb");
    
    // --- 1. MIDI Header ---
    fwrite("MThd", 1, 4, f);
    write_u32(f, 6); 
    write_u16(f, 0); // Format 0 (Single track)
    write_u16(f, 1); // 1 Track
    write_u16(f, TICKS_PER_BEAT);

    // --- 2. Track Data Start ---
    fwrite("MTrk", 1, 4, f);
    // For simplicity, let's assume a fixed length or calculate later
    // In a real project, use a temp buffer like your drum code did!

    // --- 3. Initial Setup (Delta Time 0) ---
    fputc(0x00, f); 
    fputc(0xC0, f); // Program Change on Channel 0
    fputc(AC_GUITAR, f);

    // --- 4. Playing a Strummed Chord ---
    // A "strum" means the notes start slightly apart (e.g., 20 ticks) 
    // but stay on together.
    uint32_t current_tick = 0;

    for(int i = 0; i < 4; i++) {
        write_vlq(f, 20); // 20 ticks since the last event (the strum effect)
        fputc(0x90, f);   // Note On, Channel 0
        fputc(g_major[i], f);
        fputc(100, f);    // Velocity
    }

    // --- 5. Ending the Notes ---
    // Wait one full beat, then turn them all off
    write_vlq(f, TICKS_PER_BEAT); 
    for(int i = 0; i < 4; i++) {
        // After the first Note Off, the rest happen at the same time (Delta 0)
        if (i > 0) fputc(0x00, f); 
        fputc(0x80, f); // Note Off, Channel 0
        fputc(g_major[i], f);
        fputc(0, f);
    }

    // --- 6. End of Track ---
    fputc(0x00, f); fputc(0xFF, f); fputc(0x2F, f); fputc(0x00, f);
    
    fclose(f);
    return 0;
}