// SPDX-License-Identifier: GPL-3.0-or-later
// ISO 14443-A reader transport for the high bit rate tester, on the Flipper's raw NFC poller: activation (WUPA,
// anticollision, SELECT), RATS, a PPS with the ST25R3916 switched to 212 / 424 / 848 kbit/s, and ISO 14443-4
// block exchange (chaining both ways, S(WTX)). Every frame on the air goes into the trace.
//
// All calls must come from the NFC worker's callback (nfc_start), which already holds the NFC SPI bus.
#pragma once

#include <nfc/nfc.h>
#include <lib/toolbox/bit_buffer.h>

#include "hbr_trace.h"

typedef enum {
    HbrErrNone,
    HbrErrTimeout, // the card did not answer
    HbrErrCrc, // the answer failed its CRC
    HbrErrFrame, // a broken frame on the air (incomplete, parity)
    HbrErrProtocol, // an ISO 14443-4 block that does not fit the exchange
    HbrErrOverflow, // a response longer than the buffer
    HbrErrAborted, // cancelled from the app
} HbrErr;

typedef enum {
    HbrPpsActive, // the card took the PPS and the reader switched
    HbrPpsNoTa1, // the ATS has no TA1: 106 kbit/s only
    HbrPpsNotAdvertised, // TA1 lacks the rate in one or both directions
    HbrPpsNoAnswer, // the PPS got no valid answer
    HbrPpsBadAnswer, // the answer was not the PPSS echo
} HbrPpsResult;

typedef struct {
    uint8_t uid[10];
    uint8_t uid_len;
    uint8_t atqa[2];
    uint8_t sak;
} HbrActivation;

#define HBR_HAL_SAVED_REGS 14

typedef struct {
    Nfc* nfc;
    BitBuffer* tx;
    BitBuffer* rx;
    HbrTrace* trace;
    const volatile bool* abort;

    uint8_t ats[32];
    uint8_t ats_len;
    uint16_t fsc; // largest frame the card accepts, PCB and CRC included
    uint32_t fwt_fc; // frame wait time (ATS FWI, with a floor)
    uint32_t sfgt_us; // start-up frame guard time (ATS SFGI)
    uint8_t blocknum;
    uint16_t rate; // link rate now, kbit/s

    bool regs_saved;
    uint8_t saved_regs[HBR_HAL_SAVED_REGS];

    HbrErr err;
    uint16_t err_frame; // the frame the error is about (trace frame number)
    uint32_t t_start; // CPU cycle count at the session's first frame
    uint32_t t_last; // CPU cycle count after the last card frame
} HbrHal;

void hbr_hal_init(
    HbrHal* hal,
    Nfc* nfc,
    BitBuffer* tx,
    BitBuffer* rx,
    HbrTrace* trace,
    const volatile bool* abort);

// WUPA, anticollision, SELECT. On false, *card_seen tells whether anything answered the WUPA.
bool hbr_hal_activate(HbrHal* hal, HbrActivation* act, bool* card_seen);

// RATS (FSD 256, no CID) and the ATS: frame size, frame wait time, start-up guard time.
bool hbr_hal_rats(HbrHal* hal);

// PPS to `kbps` (212 / 424 / 848) in both directions, then the reader to that rate.
HbrPpsResult hbr_hal_pps(HbrHal* hal, uint16_t kbps);

// One APDU as I-blocks; the response (without PCB) into resp.
bool hbr_hal_apdu(
    HbrHal* hal,
    const uint8_t* apdu,
    size_t len,
    uint8_t* resp,
    size_t resp_max,
    size_t* resp_len);

// S(DESELECT): the card goes to HALT.
bool hbr_hal_deselect(HbrHal* hal);

// The reader back to the register values it had before the PPS (no-op if it never switched).
void hbr_hal_restore(HbrHal* hal);

// Microseconds between the session's first frame and the last card frame.
uint32_t hbr_hal_elapsed_us(const HbrHal* hal);

// Exchanges cut off since the app started because a transmit or a reply never ended (the firmware would have waited
// for them forever). A sign of a disturbed link.
uint32_t hbr_hal_stuck_count(void);
