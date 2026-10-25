// SPDX-License-Identifier: GPL-3.0-or-later
// ISO 14443-A high bit rate tester: reads an ISO 14443-4 card at 106, 212, 424 or 848 kbit/s and shows PASS / FAIL,
// the frame count, the time and the full frame trace the Flipper kept. Made to check sniffers (e.g. the Proxmark
// with a high bit rate 14a sniffer) against a known reader: the trace here is what was on the air.
// Loop reads the card again and again until Back, each read a whole session from field on to DESELECT, and appends
// every read's trace to one file. Speed "Cycle" takes the next of the four rates on every read.
#include <furi.h>
#include <gui/gui.h>
#include <gui/view_dispatcher.h>
#include <gui/modules/popup.h>
#include <gui/modules/variable_item_list.h>
#include <nfc/nfc.h>
#include <nfc/protocols/iso14443_3a/iso14443_3a.h>
#include <notification/notification_messages.h>
#include <storage/storage.h>
#include <stdio.h>

#include "hbr_hal.h"
#include "hbr_log_view.h"
#include "hbr_trace.h"
#include "hbr_workload.h"

#define HBR_DATA_DIR      EXT_PATH("apps_data/iso14443a_hbr_tester")
#define HBR_LOOP_FAIL_RUN 10 // a loop stops itself after this many failed reads in a row
#define HBR_OUT_CHUNK     1024 // file output goes to the SD card in pieces of about this size

typedef enum {
    HbrViewMenu,
    HbrViewReading,
    HbrViewResult,
} HbrViewId;

typedef enum {
    HbrEventRead = 100,
    HbrEventLoop,
    HbrEventDone,
} HbrEvent;

#define HBR_RATES 4
#define HBR_CYCLE HBR_RATES // the speed after the four rates: the next rate on every read
static const uint16_t hbr_rates[HBR_RATES] = {106, 212, 424, 848};
// the menu's value field holds about 7 characters
static const char* const hbr_speed_text[HBR_RATES + 1] = {"106", "212", "424", "848", "Cycle"};

// A text file on the SD card, written in pieces
typedef struct {
    File* file;
    FuriString* buf;
    bool ok; // false once a write failed
} HbrOut;

typedef struct {
    Gui* gui;
    ViewDispatcher* view_dispatcher;
    NotificationApp* notifications;
    VariableItemList* menu;
    Popup* popup;
    HbrLogView* log_view;
    HbrViewId view;

    Nfc* nfc;
    BitBuffer* tx;
    BitBuffer* rx;
    HbrTrace* trace; // the read in progress
    HbrTrace* trace_last; // Loop: the last finished read, shown when the loop ends
    uint8_t* scratch;
    uint8_t speed_index; // a rate, or HBR_CYCLE
    uint8_t cycle_pos; // Cycle: the rate of the next read
    volatile bool stop_req;
    volatile bool read_done; // the worker finished a read: its result waits for the GUI thread
    bool worker_running;

    // the read in progress, or just finished
    uint32_t stuck_start; // hbr_hal_stuck_count() when it started
    uint16_t rate;
    bool pass;
    uint32_t elapsed_us;
    char reason[24];
    char saved[24];
    char popup_header[24];
    char popup_text[64];

    // Loop
    bool loop;
    uint32_t loop_start;
    uint32_t loop_stuck_start;
    uint32_t loop_reads;
    uint32_t loop_fails;
    uint32_t loop_fail_run;
    uint32_t loop_frames;
    bool led_red;
    bool last_valid;
    bool last_pass;
    uint16_t last_rate;
    char loop_stop[24]; // why the loop stopped itself
    HbrOut out; // the loop's file
} HbrApp;

// ----------------------------------------------------------------------------------------------------------------
// The read (NFC worker thread)
// ----------------------------------------------------------------------------------------------------------------

static void hbr_reason_from_error(HbrApp* app, const HbrHal* hal) {
    const char* what;
    switch(hal->err) {
    case HbrErrTimeout:
        what = "Timeout";
        break;
    case HbrErrCrc:
        what = "CRC error";
        break;
    case HbrErrFrame:
        what = "Bad frame";
        break;
    case HbrErrOverflow:
        what = "Too long";
        break;
    case HbrErrProtocol:
    default:
        what = "Protocol";
        break;
    }
    snprintf(app->reason, sizeof(app->reason), "%s @ frame %u", what, hal->err_frame);
}

static NfcCommand hbr_worker_callback(NfcEvent event, void* context) {
    HbrApp* app = context;
    // nfc_stop() only joins this thread: it ends when the callback returns Stop, so a cancel is checked first
    if(app->stop_req) return NfcCommandStop;
    if(event.type != NfcEventTypePollerReady) return NfcCommandContinue;

    hbr_trace_reset(app->trace);
    HbrHal hal;
    hbr_hal_init(&hal, app->nfc, app->tx, app->rx, app->trace, &app->stop_req);
    HbrActivation act;
    bool card_seen = false;
    if(!hbr_hal_activate(&hal, &act, &card_seen)) {
        return NfcCommandContinue; // no card yet, or one still settling: poll again
    }

    uint16_t rate = app->rate;
    bool ok = false;
    app->reason[0] = '\0';
    do {
        if(!(act.sak & 0x20)) {
            snprintf(app->reason, sizeof(app->reason), "Not 14443-4 (SAK %02X)", act.sak);
            break;
        }
        if(!hbr_hal_rats(&hal)) {
            hbr_reason_from_error(app, &hal);
            break;
        }
        if(rate > 106) {
            switch(hbr_hal_pps(&hal, rate)) {
            case HbrPpsActive:
                break;
            case HbrPpsNoTa1:
                snprintf(app->reason, sizeof(app->reason), "Card: 106 only");
                break;
            case HbrPpsNotAdvertised:
                snprintf(app->reason, sizeof(app->reason), "Card lacks %u", rate);
                break;
            case HbrPpsNoAnswer:
                snprintf(app->reason, sizeof(app->reason), "PPS not answered");
                break;
            case HbrPpsBadAnswer:
                snprintf(app->reason, sizeof(app->reason), "Bad PPS answer");
                break;
            }
            if(app->reason[0]) break;
        }
        if(!hbr_workload_run(&hal, app->scratch) || !hbr_hal_deselect(&hal)) {
            hbr_reason_from_error(app, &hal);
            break;
        }
        ok = true;
    } while(false);

    hbr_hal_restore(&hal);
    if(hal.err == HbrErrAborted || app->stop_req) return NfcCommandStop;

    if(ok && hbr_trace_incomplete(app->trace)) {
        ok = false;
        snprintf(app->reason, sizeof(app->reason), "Trace: out of memory");
    }
    app->pass = ok;
    app->elapsed_us = hbr_hal_elapsed_us(&hal);
    app->read_done = true;
    view_dispatcher_send_custom_event(app->view_dispatcher, HbrEventDone);
    return NfcCommandStop;
}

// Starts one read: the field goes on and the worker polls until a card answers. Cycle takes the next rate.
static void hbr_launch(HbrApp* app) {
    if(app->speed_index < HBR_CYCLE) {
        app->rate = hbr_rates[app->speed_index];
    } else {
        app->rate = hbr_rates[app->cycle_pos];
        app->cycle_pos = (uint8_t)((app->cycle_pos + 1) % HBR_RATES);
    }
    nfc_config(app->nfc, NfcModePoller, NfcTechIso14443a);
    nfc_set_guard_time_us(app->nfc, ISO14443_3A_GUARD_TIME_US);
    nfc_set_fdt_poll_fc(app->nfc, ISO14443_3A_FDT_POLL_FC);
    nfc_set_fdt_poll_poll_us(app->nfc, ISO14443_3A_POLL_POLL_MIN_US);
    app->stop_req = false;
    app->read_done = false;
    app->stuck_start = hbr_hal_stuck_count();
    // after a loop's swap this buffer still holds an older read: a Back before the worker's first poll must not
    // find it as the read Back cut short
    hbr_trace_reset(app->trace);
    nfc_start(app->nfc, hbr_worker_callback, app);
    app->worker_running = true;
}

// The NFC library switches the field off and waits 10 ms before its worker ends, so the card has lost power before
// the next read: every loop read is a fresh activation.
static void hbr_stop_worker(HbrApp* app) {
    if(!app->worker_running) return;
    app->stop_req = true;
    nfc_stop(app->nfc);
    app->stop_req = false;
    app->worker_running = false;
}

// ----------------------------------------------------------------------------------------------------------------
// Files
// ----------------------------------------------------------------------------------------------------------------

// Opens the next free <prefix>_NNNN_<suffix>.txt in the app's folder; its name goes into `name`.
static bool hbr_out_open(HbrOut* out, const char* prefix, const char* suffix, char* name, size_t name_size) {
    Storage* storage = furi_record_open(RECORD_STORAGE);
    storage_simply_mkdir(storage, EXT_PATH("apps_data"));
    storage_simply_mkdir(storage, HBR_DATA_DIR);
    out->file = storage_file_alloc(storage);
    out->buf = furi_string_alloc();
    out->ok = false;
    FuriString* path = furi_string_alloc();
    for(unsigned number = 1; number < 10000; number++) {
        furi_string_printf(path, "%s/%s_%04u_%s.txt", HBR_DATA_DIR, prefix, number, suffix);
        if(storage_file_exists(storage, furi_string_get_cstr(path))) continue;
        if(storage_file_open(out->file, furi_string_get_cstr(path), FSAM_WRITE, FSOM_CREATE_ALWAYS)) {
            snprintf(name, name_size, "%s_%04u_%s.txt", prefix, number, suffix);
            out->ok = true;
        }
        break;
    }
    furi_string_free(path);
    return out->ok;
}

static void hbr_out_flush(HbrOut* out) {
    size_t size = furi_string_size(out->buf);
    if(out->ok && size) {
        out->ok = storage_file_write(out->file, furi_string_get_cstr(out->buf), size) == size;
    }
    furi_string_reset(out->buf);
}

static void hbr_out_line(HbrOut* out, const FuriString* line) {
    furi_string_cat_str(out->buf, furi_string_get_cstr(line));
    furi_string_push_back(out->buf, '\n');
    if(furi_string_size(out->buf) >= HBR_OUT_CHUNK) hbr_out_flush(out);
}

// Flushes and closes the file: true when everything reached it.
static bool hbr_out_close(HbrOut* out) {
    if(!out->file) return false;
    hbr_out_flush(out);
    bool ok = out->ok;
    storage_file_close(out->file);
    storage_file_free(out->file);
    furi_string_free(out->buf);
    furi_record_close(RECORD_STORAGE);
    memset(out, 0, sizeof(*out));
    return ok;
}

static void hbr_out_trace(HbrOut* out, const HbrTrace* trace, FuriString* line) {
    HbrTraceEntry e;
    for(size_t i = 0; hbr_trace_get(trace, i, &e); i++) {
        if(e.kind == HbrTraceNote) {
            furi_string_printf(line, "           -- %.*s", (int)e.len, (const char*)e.data);
        } else {
            furi_string_printf(line, "%5u   %c  ", e.number, e.kind == HbrTraceReader ? 'R' : 'C');
            for(size_t b = 0; b < e.len; b++) {
                furi_string_cat_printf(line, b + 1 < e.len ? "%02X " : "%02X", e.data[b]);
            }
            if(e.last_bits) furi_string_cat_printf(line, " (%u bits)", e.last_bits);
            if(e.flags & HBR_TRACE_CRC_BAD) furi_string_cat_str(line, "  CRC error");
        }
        hbr_out_line(out, line);
    }
}

static void hbr_cat_ms(FuriString* line, uint32_t us) {
    furi_string_cat_printf(
        line, "%lu.%lu ms", (unsigned long)(us / 1000), (unsigned long)((us % 1000) / 100));
}

// A single read goes to read_NNNN_<rate>.txt.
static void hbr_save(HbrApp* app) {
    HbrOut out = {0};
    char rate[8];
    snprintf(rate, sizeof(rate), "%u", app->rate);
    if(hbr_out_open(&out, "read", rate, app->saved, sizeof(app->saved))) {
        FuriString* line = furi_string_alloc();
        furi_string_printf(line, "ISO 14443-A high bit rate test, %s", app->saved);
        hbr_out_line(&out, line);
        furi_string_printf(line, "Result: %s", app->pass ? "PASS" : "FAIL");
        if(!app->pass) furi_string_cat_printf(line, " (%s)", app->reason);
        hbr_out_line(&out, line);
        furi_string_printf(line, "Speed: %u kbit/s", app->rate);
        hbr_out_line(&out, line);
        furi_string_printf(line, "Frames: %u", hbr_trace_frames(app->trace));
        hbr_out_line(&out, line);
        furi_string_set_str(line, "Time: ");
        hbr_cat_ms(line, app->elapsed_us);
        furi_string_cat_str(line, " (first frame to the last card frame)");
        hbr_out_line(&out, line);
        uint32_t stuck = hbr_hal_stuck_count() - app->stuck_start;
        if(stuck) {
            furi_string_printf(line, "Frames that never ended: %lu (cut off)", (unsigned long)stuck);
            hbr_out_line(&out, line);
        }
        furi_string_set_str(line, "\n    #  dir  bytes (CRC included; R = Flipper, C = card)");
        hbr_out_line(&out, line);
        hbr_out_trace(&out, app->trace, line);
        furi_string_free(line);
    }
    if(!hbr_out_close(&out)) app->saved[0] = '\0';
}

// ----------------------------------------------------------------------------------------------------------------
// Screens
// ----------------------------------------------------------------------------------------------------------------

typedef char HbrLine[HBR_TRACE_LINE_CHARS + 2];

static void hbr_switch(HbrApp* app, HbrViewId view) {
    app->view = view;
    view_dispatcher_switch_to_view(app->view_dispatcher, view);
}

static void hbr_show(HbrApp* app, HbrLine* lines, size_t n, const HbrTrace* trace) {
    const char* header[HBR_LOG_HEADER_LINES];
    for(size_t i = 0; i < n; i++)
        header[i] = lines[i];
    hbr_log_view_set(app->log_view, header, n, trace);
    hbr_switch(app, HbrViewResult);
}

static void hbr_start_read(HbrApp* app) {
    hbr_launch(app);
    popup_reset(app->popup);
    popup_set_header(app->popup, "Reading", 64, 4, AlignCenter, AlignTop);
    snprintf(
        app->popup_text, sizeof(app->popup_text), "%u kbit/s\nPlace the card\nBack: cancel", app->rate);
    popup_set_text(app->popup, app->popup_text, 64, 22, AlignCenter, AlignTop);
    hbr_switch(app, HbrViewReading);
    notification_message(app->notifications, &sequence_blink_start_cyan);
}

static void hbr_finish_read(HbrApp* app) {
    notification_message(app->notifications, &sequence_blink_stop);
    hbr_save(app);
    notification_message(
        app->notifications, app->pass ? &sequence_blink_green_100 : &sequence_blink_red_100);
    notification_message(app->notifications, &sequence_single_vibro);

    HbrLine lines[HBR_LOG_HEADER_LINES];
    size_t n = 0;
    snprintf(lines[n++], sizeof(lines[0]), "%s  %u kbit/s", app->pass ? "PASS" : "FAIL", app->rate);
    snprintf(
        lines[n++],
        sizeof(lines[0]),
        "%u fr  %lu.%lu ms",
        hbr_trace_frames(app->trace),
        (unsigned long)(app->elapsed_us / 1000),
        (unsigned long)((app->elapsed_us % 1000) / 100));
    if(!app->pass) strlcpy(lines[n++], app->reason, sizeof(lines[0]));
    strlcpy(lines[n++], app->saved[0] ? app->saved : "not saved (SD?)", sizeof(lines[0]));
    hbr_show(app, lines, n, app->trace);
}

// ----------------------------------------------------------------------------------------------------------------
// Loop
// ----------------------------------------------------------------------------------------------------------------

static uint32_t hbr_loop_seconds(const HbrApp* app) {
    return (furi_get_tick() - app->loop_start) / furi_kernel_get_tick_frequency();
}

static void hbr_loop_popup(HbrApp* app) {
    if(app->speed_index < HBR_CYCLE) {
        snprintf(app->popup_header, sizeof(app->popup_header), "Loop  %u kbit/s", app->rate);
    } else {
        snprintf(app->popup_header, sizeof(app->popup_header), "Loop  Cycle: %u", app->rate);
    }
    uint32_t s = hbr_loop_seconds(app);
    snprintf(
        app->popup_text,
        sizeof(app->popup_text),
        "Reads %lu   FAIL %lu\nFrames %lu\n%lu:%02lu   Back: stop",
        (unsigned long)app->loop_reads,
        (unsigned long)app->loop_fails,
        (unsigned long)app->loop_frames,
        (unsigned long)(s / 60),
        (unsigned long)(s % 60));
    popup_set_header(app->popup, app->popup_header, 64, 4, AlignCenter, AlignTop);
    popup_set_text(app->popup, app->popup_text, 64, 22, AlignCenter, AlignTop);
}

// A finished loop read: counted and appended to the file; its trace is the one shown when the loop ends.
static void hbr_loop_record(HbrApp* app) {
    app->loop_reads++;
    app->loop_frames += hbr_trace_frames(app->trace);
    if(app->pass) {
        app->loop_fail_run = 0;
    } else {
        app->loop_fails++;
        app->loop_fail_run++;
    }

    FuriString* line = furi_string_alloc();
    furi_string_printf(
        line,
        "\n=== read %lu: %u kbit/s, %s",
        (unsigned long)app->loop_reads,
        app->rate,
        app->pass ? "PASS" : "FAIL");
    if(!app->pass) furi_string_cat_printf(line, " (%s)", app->reason);
    furi_string_cat_printf(line, ", %u frames, ", hbr_trace_frames(app->trace));
    hbr_cat_ms(line, app->elapsed_us);
    hbr_out_line(&app->out, line);
    hbr_out_trace(&app->out, app->trace, line);
    hbr_out_flush(&app->out);
    furi_string_free(line);

    HbrTrace* done = app->trace;
    app->trace = app->trace_last;
    app->trace_last = done;
    app->last_valid = true;
    app->last_pass = app->pass;
    app->last_rate = app->rate;
}

static bool hbr_card_answered(const HbrTrace* trace) {
    HbrTraceEntry e;
    for(size_t i = 0; hbr_trace_get(trace, i, &e); i++) {
        if(e.kind == HbrTraceCard) return true;
    }
    return false;
}

static void hbr_finish_loop(HbrApp* app) {
    bool was_reading = app->worker_running;
    hbr_stop_worker(app);
    // a read that ended just as Back was pressed still counts: a sniffer saw it too
    if(app->read_done) {
        app->read_done = false;
        hbr_loop_record(app);
    } else if(was_reading && app->out.file && hbr_card_answered(app->trace)) {
        // a read Back cut short: in the file for the comparison, not counted
        FuriString* line = furi_string_alloc();
        furi_string_printf(
            line, "\n=== stopped by Back, not counted: %u frames", hbr_trace_frames(app->trace));
        hbr_out_line(&app->out, line);
        hbr_out_trace(&app->out, app->trace, line);
        furi_string_free(line);
    }
    app->loop = false;
    uint32_t s = hbr_loop_seconds(app);

    if(app->out.file) {
        FuriString* line = furi_string_alloc();
        furi_string_printf(
            line,
            "\nTotal: %lu reads, %lu PASS, %lu FAIL, %lu frames, %lu:%02lu",
            (unsigned long)app->loop_reads,
            (unsigned long)(app->loop_reads - app->loop_fails),
            (unsigned long)app->loop_fails,
            (unsigned long)app->loop_frames,
            (unsigned long)(s / 60),
            (unsigned long)(s % 60));
        uint32_t stuck = hbr_hal_stuck_count() - app->loop_stuck_start;
        if(stuck) furi_string_cat_printf(line, ", %lu frames never ended (cut off)", (unsigned long)stuck);
        if(app->loop_stop[0]) furi_string_cat_printf(line, ", stopped: %s", app->loop_stop);
        hbr_out_line(&app->out, line);
        furi_string_free(line);
        if(!hbr_out_close(&app->out) && app->saved[0] && !app->loop_stop[0]) {
            strlcpy(app->loop_stop, "SD write failed", sizeof(app->loop_stop));
        }
    }

    bool good = app->loop_reads > 0 && app->loop_fails == 0 && !app->loop_stop[0] && app->saved[0];
    notification_message(app->notifications, &sequence_blink_stop);
    notification_message(
        app->notifications, good ? &sequence_blink_green_100 : &sequence_blink_red_100);
    notification_message(app->notifications, &sequence_single_vibro);

    HbrLine lines[HBR_LOG_HEADER_LINES];
    size_t n = 0;
    snprintf(
        lines[n++],
        sizeof(lines[0]),
        "LOOP  %s%s",
        hbr_speed_text[app->speed_index],
        app->speed_index < HBR_CYCLE ? " kbit/s" : "");
    snprintf(
        lines[n++],
        sizeof(lines[0]),
        "%lu reads  %lu FAIL",
        (unsigned long)app->loop_reads,
        (unsigned long)app->loop_fails);
    snprintf(
        lines[n++],
        sizeof(lines[0]),
        "%lu fr  %lu:%02lu",
        (unsigned long)app->loop_frames,
        (unsigned long)(s / 60),
        (unsigned long)(s % 60));
    strlcpy(lines[n++], app->saved[0] ? app->saved : "not saved (SD?)", sizeof(lines[0]));
    if(app->loop_stop[0]) strlcpy(lines[n++], app->loop_stop, sizeof(lines[0]));
    if(app->last_valid) {
        snprintf(
            lines[n++],
            sizeof(lines[0]),
            "Last read: %s %u",
            app->last_pass ? "PASS" : "FAIL",
            app->last_rate);
    }
    hbr_show(app, lines, n, app->last_valid ? app->trace_last : NULL);
}

static void hbr_start_loop(HbrApp* app) {
    app->loop = true;
    app->loop_start = furi_get_tick();
    app->loop_stuck_start = hbr_hal_stuck_count();
    app->loop_reads = app->loop_fails = app->loop_fail_run = app->loop_frames = 0;
    app->cycle_pos = 0; // Cycle: a loop always starts at 106
    app->led_red = false;
    app->last_valid = false;
    app->loop_stop[0] = '\0';
    app->saved[0] = '\0';

    char suffix[8];
    if(app->speed_index < HBR_CYCLE) {
        snprintf(suffix, sizeof(suffix), "%u", hbr_rates[app->speed_index]);
    } else {
        strlcpy(suffix, "cycle", sizeof(suffix));
    }
    if(!hbr_out_open(&app->out, "loop", suffix, app->saved, sizeof(app->saved))) {
        app->saved[0] = '\0';
        hbr_finish_loop(app); // nowhere to keep the reads
        return;
    }
    FuriString* line = furi_string_alloc();
    furi_string_printf(
        line,
        "ISO 14443-A high bit rate test, %s, speed %s%s",
        app->saved,
        hbr_speed_text[app->speed_index],
        app->speed_index < HBR_CYCLE ? " kbit/s" : "");
    hbr_out_line(&app->out, line);
    furi_string_set_str(
        line,
        "Each read: field on to DESELECT. R = Flipper, C = card, CRC included; "
        "time = first frame to the last card frame");
    hbr_out_line(&app->out, line);
    furi_string_free(line);

    hbr_launch(app);
    popup_reset(app->popup);
    hbr_loop_popup(app);
    hbr_switch(app, HbrViewReading);
    notification_message(app->notifications, &sequence_blink_start_cyan);
}

static void hbr_loop_step(HbrApp* app) {
    hbr_loop_record(app);
    if(!app->out.ok) {
        strlcpy(app->loop_stop, "SD write failed", sizeof(app->loop_stop));
    } else if(app->loop_fail_run >= HBR_LOOP_FAIL_RUN) {
        snprintf(app->loop_stop, sizeof(app->loop_stop), "%u FAILs in a row", HBR_LOOP_FAIL_RUN);
    }
    if(app->loop_stop[0]) {
        hbr_finish_loop(app);
        return;
    }
    // the LED blinks red after a failed read, cyan after a good one
    if(app->led_red == app->pass) {
        app->led_red = !app->pass;
        notification_message(
            app->notifications, app->pass ? &sequence_blink_start_cyan : &sequence_blink_start_red);
    }
    hbr_launch(app);
    hbr_loop_popup(app);
}

// ----------------------------------------------------------------------------------------------------------------
// Events
// ----------------------------------------------------------------------------------------------------------------

static bool hbr_custom_event(void* context, uint32_t event) {
    HbrApp* app = context;
    switch(event) {
    case HbrEventRead:
        hbr_start_read(app);
        return true;
    case HbrEventLoop:
        hbr_start_loop(app);
        return true;
    case HbrEventDone:
        // a read can end just as Back stops it: Back has dealt with it then
        if(app->view != HbrViewReading || !app->read_done) return true;
        hbr_stop_worker(app);
        app->read_done = false;
        if(app->loop) {
            hbr_loop_step(app);
        } else {
            hbr_finish_read(app);
        }
        return true;
    default:
        return false;
    }
}

static bool hbr_back(void* context) {
    HbrApp* app = context;
    switch(app->view) {
    case HbrViewReading:
        if(app->loop) {
            hbr_finish_loop(app);
            return true;
        }
        hbr_stop_worker(app);
        app->read_done = false; // a read that ended just now: the cancel wins
        notification_message(app->notifications, &sequence_blink_stop);
        hbr_switch(app, HbrViewMenu);
        return true;
    case HbrViewResult:
        hbr_switch(app, HbrViewMenu);
        return true;
    default:
        return false; // menu: leave the app
    }
}

static void hbr_speed_changed(VariableItem* item) {
    HbrApp* app = variable_item_get_context(item);
    app->speed_index = variable_item_get_current_value_index(item);
    app->cycle_pos = 0;
    variable_item_set_current_value_text(item, hbr_speed_text[app->speed_index]);
}

static void hbr_menu_enter(void* context, uint32_t index) {
    HbrApp* app = context;
    if(index == 1) view_dispatcher_send_custom_event(app->view_dispatcher, HbrEventRead);
    if(index == 2) view_dispatcher_send_custom_event(app->view_dispatcher, HbrEventLoop);
}

// ----------------------------------------------------------------------------------------------------------------
// App
// ----------------------------------------------------------------------------------------------------------------

int32_t hbr_tester_app(void* p) {
    UNUSED(p);
    HbrApp* app = malloc(sizeof(HbrApp));
    memset(app, 0, sizeof(HbrApp));
    app->speed_index = 3;

    app->gui = furi_record_open(RECORD_GUI);
    app->notifications = furi_record_open(RECORD_NOTIFICATION);
    app->view_dispatcher = view_dispatcher_alloc();
    view_dispatcher_set_event_callback_context(app->view_dispatcher, app);
    view_dispatcher_set_custom_event_callback(app->view_dispatcher, hbr_custom_event);
    view_dispatcher_set_navigation_event_callback(app->view_dispatcher, hbr_back);
    view_dispatcher_attach_to_gui(app->view_dispatcher, app->gui, ViewDispatcherTypeFullscreen);

    app->menu = variable_item_list_alloc();
    VariableItem* speed = variable_item_list_add(
        app->menu, "Speed", COUNT_OF(hbr_speed_text), hbr_speed_changed, app);
    variable_item_set_current_value_index(speed, app->speed_index);
    variable_item_set_current_value_text(speed, hbr_speed_text[app->speed_index]);
    variable_item_list_add(app->menu, "Read", 0, NULL, app);
    variable_item_list_add(app->menu, "Loop", 0, NULL, app);
    variable_item_list_set_enter_callback(app->menu, hbr_menu_enter, app);
    view_dispatcher_add_view(
        app->view_dispatcher, HbrViewMenu, variable_item_list_get_view(app->menu));

    app->popup = popup_alloc();
    view_dispatcher_add_view(app->view_dispatcher, HbrViewReading, popup_get_view(app->popup));
    app->log_view = hbr_log_view_alloc();
    view_dispatcher_add_view(
        app->view_dispatcher, HbrViewResult, hbr_log_view_get_view(app->log_view));

    app->nfc = nfc_alloc();
    app->tx = bit_buffer_alloc(300);
    app->rx = bit_buffer_alloc(300);
    app->trace = hbr_trace_alloc();
    app->trace_last = hbr_trace_alloc();
    app->scratch = malloc(HBR_WORKLOAD_SCRATCH);

    hbr_switch(app, HbrViewMenu);
    view_dispatcher_run(app->view_dispatcher);

    hbr_stop_worker(app);
    hbr_out_close(&app->out);
    notification_message(app->notifications, &sequence_blink_stop);
    view_dispatcher_remove_view(app->view_dispatcher, HbrViewMenu);
    view_dispatcher_remove_view(app->view_dispatcher, HbrViewReading);
    view_dispatcher_remove_view(app->view_dispatcher, HbrViewResult);
    variable_item_list_free(app->menu);
    popup_free(app->popup);
    hbr_log_view_free(app->log_view);
    view_dispatcher_free(app->view_dispatcher);

    free(app->scratch);
    hbr_trace_free(app->trace);
    hbr_trace_free(app->trace_last);
    bit_buffer_free(app->tx);
    bit_buffer_free(app->rx);
    nfc_free(app->nfc);
    furi_record_close(RECORD_NOTIFICATION);
    furi_record_close(RECORD_GUI);
    free(app);
    return 0;
}
