// SPDX-License-Identifier: GPL-3.0-or-later
#include "hbr_log_view.h"

#include <furi.h>
#include <gui/elements.h>
#include <string.h>

#define HBR_LOG_SCREEN_LINES 8
#define HBR_LOG_LINE_HEIGHT 8

struct HbrLogView {
    View* view;
};

typedef struct {
    char header[HBR_LOG_HEADER_LINES][HBR_TRACE_LINE_CHARS + 2];
    size_t header_lines;
    const HbrTrace* trace;
    size_t top;
} HbrLogModel;

static size_t hbr_log_total(const HbrLogModel* m) {
    return m->header_lines + (m->trace ? hbr_trace_lines(m->trace) : 0);
}

static size_t hbr_log_max_top(const HbrLogModel* m) {
    size_t total = hbr_log_total(m);
    return total > HBR_LOG_SCREEN_LINES ? total - HBR_LOG_SCREEN_LINES : 0;
}

static void hbr_log_draw(Canvas* canvas, void* model) {
    HbrLogModel* m = model;
    canvas_clear(canvas);
    canvas_set_font(canvas, FontKeyboard);
    size_t total = hbr_log_total(m);
    char line[HBR_TRACE_LINE_CHARS + 12];
    for(size_t i = 0; i < HBR_LOG_SCREEN_LINES; i++) {
        size_t n = m->top + i;
        if(n >= total) break;
        if(n < m->header_lines) {
            strlcpy(line, m->header[n], sizeof(line));
        } else {
            hbr_trace_line(m->trace, n - m->header_lines, line, sizeof(line));
        }
        canvas_draw_str(canvas, 0, (int32_t)(HBR_LOG_LINE_HEIGHT * (i + 1) - 1), line);
    }
    if(total > HBR_LOG_SCREEN_LINES) {
        elements_scrollbar(canvas, m->top, hbr_log_max_top(m) + 1);
    }
}

static bool hbr_log_input(InputEvent* event, void* context) {
    HbrLogView* log_view = context;
    if(event->key == InputKeyBack) return false;
    if(event->type != InputTypeShort && event->type != InputTypeRepeat) return true;
    with_view_model(
        log_view->view,
        HbrLogModel * m,
        {
            size_t max_top = hbr_log_max_top(m);
            switch(event->key) {
            case InputKeyUp:
                if(m->top > 0) m->top--;
                break;
            case InputKeyDown:
                if(m->top < max_top) m->top++;
                break;
            case InputKeyLeft:
                m->top = m->top > HBR_LOG_SCREEN_LINES ? m->top - HBR_LOG_SCREEN_LINES : 0;
                break;
            case InputKeyRight:
                m->top = m->top + HBR_LOG_SCREEN_LINES < max_top ? m->top + HBR_LOG_SCREEN_LINES : max_top;
                break;
            case InputKeyOk:
                m->top = (m->top == 0) ? max_top : 0; // OK: jump to the end and back
                break;
            default:
                break;
            }
        },
        true);
    return true;
}

HbrLogView* hbr_log_view_alloc(void) {
    HbrLogView* log_view = malloc(sizeof(HbrLogView));
    log_view->view = view_alloc();
    view_allocate_model(log_view->view, ViewModelTypeLocking, sizeof(HbrLogModel));
    view_set_context(log_view->view, log_view);
    view_set_draw_callback(log_view->view, hbr_log_draw);
    view_set_input_callback(log_view->view, hbr_log_input);
    return log_view;
}

void hbr_log_view_free(HbrLogView* log_view) {
    view_free(log_view->view);
    free(log_view);
}

View* hbr_log_view_get_view(HbrLogView* log_view) {
    return log_view->view;
}

void hbr_log_view_set(
    HbrLogView* log_view,
    const char* const header[],
    size_t header_lines,
    const HbrTrace* trace) {
    with_view_model(
        log_view->view,
        HbrLogModel * m,
        {
            if(header_lines > HBR_LOG_HEADER_LINES) header_lines = HBR_LOG_HEADER_LINES;
            for(size_t i = 0; i < header_lines; i++) {
                strlcpy(m->header[i], header[i], sizeof(m->header[i]));
            }
            m->header_lines = header_lines;
            m->trace = trace;
            m->top = 0;
        },
        true);
}
