// SPDX-License-Identifier: GPL-3.0-or-later
// Result screen: a few summary lines, then the whole frame trace, scrolled line by line. Lines are formatted from
// the compact trace as they are drawn, so a long read never needs one big text buffer.
#pragma once

#include <gui/view.h>

#include "hbr_trace.h"

#define HBR_LOG_HEADER_LINES 6

typedef struct HbrLogView HbrLogView;

HbrLogView* hbr_log_view_alloc(void);
void hbr_log_view_free(HbrLogView* log_view);
View* hbr_log_view_get_view(HbrLogView* log_view);

// `trace` is borrowed and must stay unchanged while the view shows it.
void hbr_log_view_set(
    HbrLogView* log_view,
    const char* const header[],
    size_t header_lines,
    const HbrTrace* trace);
