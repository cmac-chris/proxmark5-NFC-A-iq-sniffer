// SPDX-License-Identifier: GPL-3.0-or-later
// What a read exchanges once the link is up. MIFARE DESFire cards get the full walk: versions, key settings and
// versions, free memory, the originality signature, applications, file settings, and the contents of every file that
// is free to read (read or read & write access E). Other cards get ISO 7816 SELECTs of well-known applications and an
// NFC Forum type 4 tag's NDEF file when it is free to read. Nothing is written or authenticated. A card's error status
// is an answer like any other: only transport errors fail a read.
#pragma once

#include "hbr_hal.h"

#define HBR_WORKLOAD_SCRATCH 1024

// `scratch` holds HBR_WORKLOAD_SCRATCH bytes. False on a transport error (hal->err says which).
bool hbr_workload_run(HbrHal* hal, uint8_t* scratch);
