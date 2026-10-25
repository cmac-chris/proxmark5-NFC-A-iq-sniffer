// SPDX-License-Identifier: GPL-3.0-or-later
#include "hbr_trace.h"

#include <furi.h>
#include <stdio.h>
#include <string.h>

// Each entry is an 8-byte header followed by its bytes, in one growable pool:
//   [kind][flags][last_bits][reserved][len lo][len hi][number lo][number hi] data...
// plus an index of entry offsets and of the screen line each entry starts on.
#define HBR_ENTRY_HDR 8
#define HBR_LINE_BYTES 5 // frame bytes per screen line

struct HbrTrace {
    uint8_t* pool;
    size_t pool_len;
    size_t pool_cap;
    uint32_t* offsets; // entry -> pool offset
    uint32_t* first_line; // entry -> its first screen line
    size_t entries;
    size_t cap;
    size_t frames;
    size_t lines;
    bool incomplete;
};

HbrTrace* hbr_trace_alloc(void) {
    HbrTrace* trace = malloc(sizeof(HbrTrace));
    memset(trace, 0, sizeof(HbrTrace));
    return trace;
}

void hbr_trace_free(HbrTrace* trace) {
    if(!trace) return;
    free(trace->pool);
    free(trace->offsets);
    free(trace->first_line);
    free(trace);
}

void hbr_trace_reset(HbrTrace* trace) {
    trace->pool_len = 0;
    trace->entries = 0;
    trace->frames = 0;
    trace->lines = 0;
    trace->incomplete = false;
}

static size_t hbr_entry_lines(HbrTraceKind kind, size_t len, uint8_t flags) {
    if(kind == HbrTraceNote) return 1;
    size_t lines = (len + HBR_LINE_BYTES - 1) / HBR_LINE_BYTES;
    if(lines == 0) lines = 1;
    if(flags & HBR_TRACE_CRC_BAD) lines++;
    return lines;
}

// heap left for the rest of the app when the trace grows
#define HBR_HEAP_RESERVE 8192

// Grows a buffer to hold at least `need` elements of `elem` bytes (doubling). Returns false when the heap is too
// short: the Flipper's allocator halts the system instead of failing, so the free block is checked first.
static bool hbr_grow(void** buf, size_t* cap, size_t need, size_t elem) {
    if(need <= *cap) return true;
    size_t ncap = *cap ? *cap : 64;
    while(ncap < need)
        ncap *= 2;
    if(memmgr_heap_get_max_free_block() < ncap * elem + HBR_HEAP_RESERVE) {
        ncap = need;
        if(memmgr_heap_get_max_free_block() < ncap * elem + HBR_HEAP_RESERVE) return false;
    }
    void* p = realloc(*buf, ncap * elem);
    if(!p) return false;
    *buf = p;
    *cap = ncap;
    return true;
}

static bool hbr_trace_add(
    HbrTrace* trace,
    HbrTraceKind kind,
    const uint8_t* data,
    size_t len,
    uint8_t last_bits,
    uint8_t flags) {
    if(trace->incomplete) return false;
    if(len > 0xFFFF) len = 0xFFFF;
    size_t idx_cap = trace->cap;
    size_t idx_cap2 = trace->cap;
    if(!hbr_grow((void**)&trace->offsets, &idx_cap, trace->entries + 1, sizeof(uint32_t)) ||
       !hbr_grow((void**)&trace->first_line, &idx_cap2, trace->entries + 1, sizeof(uint32_t)) ||
       !hbr_grow((void**)&trace->pool, &trace->pool_cap, trace->pool_len + HBR_ENTRY_HDR + len, 1)) {
        trace->incomplete = true;
        return false;
    }
    trace->cap = idx_cap < idx_cap2 ? idx_cap : idx_cap2;

    uint16_t number = (kind == HbrTraceNote) ? 0 : (uint16_t)(trace->frames + 1);
    uint8_t* p = trace->pool + trace->pool_len;
    p[0] = (uint8_t)kind;
    p[1] = flags;
    p[2] = last_bits;
    p[3] = 0;
    p[4] = (uint8_t)(len & 0xFF);
    p[5] = (uint8_t)(len >> 8);
    p[6] = (uint8_t)(number & 0xFF);
    p[7] = (uint8_t)(number >> 8);
    if(len) memcpy(p + HBR_ENTRY_HDR, data, len);

    trace->offsets[trace->entries] = (uint32_t)trace->pool_len;
    trace->first_line[trace->entries] = (uint32_t)trace->lines;
    trace->pool_len += HBR_ENTRY_HDR + len;
    trace->entries++;
    trace->lines += hbr_entry_lines(kind, len, flags);
    if(kind != HbrTraceNote) trace->frames++;
    return true;
}

bool hbr_trace_add_frame(
    HbrTrace* trace,
    HbrTraceKind kind,
    const uint8_t* data,
    size_t len,
    uint8_t last_bits,
    uint8_t flags) {
    return hbr_trace_add(trace, kind, data, len, last_bits, flags);
}

bool hbr_trace_add_note(HbrTrace* trace, const char* text) {
    return hbr_trace_add(trace, HbrTraceNote, (const uint8_t*)text, strlen(text), 0, 0);
}

size_t hbr_trace_entries(const HbrTrace* trace) {
    return trace->entries;
}

size_t hbr_trace_frames(const HbrTrace* trace) {
    return trace->frames;
}

bool hbr_trace_incomplete(const HbrTrace* trace) {
    return trace->incomplete;
}

bool hbr_trace_get(const HbrTrace* trace, size_t index, HbrTraceEntry* out) {
    if(index >= trace->entries) return false;
    const uint8_t* p = trace->pool + trace->offsets[index];
    out->kind = (HbrTraceKind)p[0];
    out->flags = p[1];
    out->last_bits = p[2];
    out->len = (uint16_t)(p[4] | (p[5] << 8));
    out->number = (uint16_t)(p[6] | (p[7] << 8));
    out->data = p + HBR_ENTRY_HDR;
    return true;
}

size_t hbr_trace_lines(const HbrTrace* trace) {
    return trace->lines;
}

// The entry holding screen line `line` (binary search over first_line).
static size_t hbr_entry_for_line(const HbrTrace* trace, size_t line) {
    size_t lo = 0, hi = trace->entries;
    while(hi - lo > 1) {
        size_t mid = (lo + hi) / 2;
        if(trace->first_line[mid] <= line)
            lo = mid;
        else
            hi = mid;
    }
    return lo;
}

void hbr_trace_line(const HbrTrace* trace, size_t line, char* out, size_t out_size) {
    out[0] = '\0';
    if(line >= trace->lines || trace->entries == 0) return;
    size_t index = hbr_entry_for_line(trace, line);
    HbrTraceEntry e;
    if(!hbr_trace_get(trace, index, &e)) return;
    size_t sub = line - trace->first_line[index];

    if(e.kind == HbrTraceNote) {
        // "     - text", cut to the screen width
        int n = snprintf(out, out_size, "     - %.*s", (int)e.len, (const char*)e.data);
        if(n > (int)HBR_TRACE_LINE_CHARS - 1 && out_size > HBR_TRACE_LINE_CHARS) {
            out[HBR_TRACE_LINE_CHARS - 1] = '\0';
        }
        return;
    }

    size_t data_lines = (e.len + HBR_LINE_BYTES - 1) / HBR_LINE_BYTES;
    if(data_lines == 0) data_lines = 1;
    if(sub >= data_lines) {
        snprintf(out, out_size, "      ^ CRC error");
        return;
    }
    size_t pos;
    if(sub == 0) {
        pos = (size_t)snprintf(
            out, out_size, "%4u%c ", e.number % 10000, e.kind == HbrTraceReader ? 'R' : 'C');
    } else {
        pos = (size_t)snprintf(out, out_size, "      ");
    }
    size_t start = sub * HBR_LINE_BYTES;
    size_t end = start + HBR_LINE_BYTES;
    if(end > e.len) end = e.len;
    for(size_t i = start; i < end && pos + 3 < out_size; i++) {
        pos += (size_t)snprintf(out + pos, out_size - pos, i + 1 < end ? "%02X " : "%02X", e.data[i]);
    }
    if(e.last_bits && sub == data_lines - 1 && pos + 6 < out_size) {
        snprintf(out + pos, out_size - pos, " (%ub)", e.last_bits);
    }
}
