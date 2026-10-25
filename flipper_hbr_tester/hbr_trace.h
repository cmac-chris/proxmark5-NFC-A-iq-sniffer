// SPDX-License-Identifier: GPL-3.0-or-later
// Frame trace kept by the reader: every frame on air (as the Flipper sent / received it, CRC included) and short
// notes (timeouts, rate switches), in a compact growable store, so a whole read fits without truncation.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    HbrTraceReader, // a frame the Flipper sent
    HbrTraceCard, // a frame the card sent
    HbrTraceNote, // not a frame: a short note
} HbrTraceKind;

#define HBR_TRACE_CRC_BAD 0x01 // the frame failed its CRC

typedef struct {
    HbrTraceKind kind;
    uint8_t flags;
    uint8_t last_bits; // bits in the last byte (1..7) of a short frame, 0 for whole bytes
    uint16_t len; // bytes (notes: characters)
    uint16_t number; // frame number, 1-based (notes: 0)
    const uint8_t* data;
} HbrTraceEntry;

typedef struct HbrTrace HbrTrace;

HbrTrace* hbr_trace_alloc(void);
void hbr_trace_free(HbrTrace* trace);
void hbr_trace_reset(HbrTrace* trace);

// false when out of memory (the trace is then marked incomplete)
bool hbr_trace_add_frame(
    HbrTrace* trace,
    HbrTraceKind kind,
    const uint8_t* data,
    size_t len,
    uint8_t last_bits,
    uint8_t flags);
bool hbr_trace_add_note(HbrTrace* trace, const char* text);

size_t hbr_trace_entries(const HbrTrace* trace);
size_t hbr_trace_frames(const HbrTrace* trace);
bool hbr_trace_incomplete(const HbrTrace* trace);
bool hbr_trace_get(const HbrTrace* trace, size_t index, HbrTraceEntry* out);

// Screen lines (5 bytes of a frame per line, 20 characters): total count, and one line by number.
#define HBR_TRACE_LINE_CHARS 21
size_t hbr_trace_lines(const HbrTrace* trace);
void hbr_trace_line(const HbrTrace* trace, size_t line, char* out, size_t out_size);
