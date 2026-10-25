// SPDX-License-Identifier: GPL-3.0-or-later
#include "hbr_hal.h"

#include <furi.h>
#include <furi_hal.h>
#include <lib/drivers/st25r3916.h>
#include <lib/drivers/st25r3916_reg.h>
#include <nfc/helpers/iso14443_crc.h>
#include <nfc/protocols/iso14443_3a/iso14443_3a.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define HBR_FC_PER_MS 13560u
// Every wait after activation is at least this long: lenient with slow cards, still quick to notice a card that left.
#define HBR_FWT_FLOOR_FC (200u * HBR_FC_PER_MS)
#define HBR_FWT_MAX_FC (5000u * HBR_FC_PER_MS)
#define HBR_DELTA_FWT_FC 49152u // ISO 14443-4 delta FWT

// ISO 14443-4 PCB bytes (no CID, no NAD)
#define PCB_I 0x02
#define PCB_I_CHAIN 0x10
#define PCB_R_ACK 0xA2
#define PCB_S_DESELECT 0xC2
#define PCB_S_WTX 0xF2
#define PCB_BLOCKNUM 0x01

static uint32_t hbr_cycles(void) {
    return furi_hal_cortex_timer_get(0).start;
}

static void hbr_note(HbrHal* hal, const char* fmt, ...) {
    char text[32];
    va_list args;
    va_start(args, fmt);
    vsnprintf(text, sizeof(text), fmt, args);
    va_end(args);
    hbr_trace_add_note(hal->trace, text);
}

static void hbr_log(HbrHal* hal, HbrTraceKind kind, const BitBuffer* buf, uint8_t flags) {
    size_t bits = bit_buffer_get_size(buf);
    hbr_trace_add_frame(
        hal->trace,
        kind,
        bit_buffer_get_data(buf),
        bit_buffer_get_size_bytes(buf),
        (uint8_t)(bits % 8),
        flags);
    if(kind == HbrTraceCard) hal->t_last = hbr_cycles();
}

static bool hbr_fail(HbrHal* hal, HbrErr err) {
    hal->err = err;
    hal->err_frame = (uint16_t)hbr_trace_frames(hal->trace);
    return false;
}

static bool hbr_protocol_error(HbrHal* hal) {
    hbr_note(hal, "unexpected block");
    return hbr_fail(hal, HbrErrProtocol);
}

void hbr_hal_init(
    HbrHal* hal,
    Nfc* nfc,
    BitBuffer* tx,
    BitBuffer* rx,
    HbrTrace* trace,
    const volatile bool* abort) {
    memset(hal, 0, sizeof(*hal));
    hal->nfc = nfc;
    hal->tx = tx;
    hal->rx = rx;
    hal->trace = trace;
    hal->abort = abort;
    hal->fsc = 32; // ISO default (FSCI 2) until the ATS says otherwise
    hal->fwt_fc = HBR_FWT_FLOOR_FC;
    hal->rate = 106;
}

// ----------------------------------------------------------------------------------------------------------------
// One exchange: nfc_poller_trx() with every wait bounded. The firmware's waits for the end of a transmit and of a
// reply that has started have no timeout: a reply that never ended (seen with a marginal link) hung the NFC worker,
// and with it the app, until a reboot.
// ----------------------------------------------------------------------------------------------------------------

#define HBR_STEP_MS  100u // longest a transmit or a started reply may run (256 bytes at 106 kbit/s: 22 ms)
#define HBR_SLICE_MS 50u // waits are cut in slices of this, so a cancel is seen

typedef enum {
    HbrTxFrame, // hal->tx as it is, parity by the chip
    HbrTxWupa,
    HbrTxSdd, // an anticollision frame
} HbrTx;

typedef enum {
    HbrTrxOk,
    HbrTrxNoAnswer,
    HbrTrxBroken, // incomplete frame, parity
    HbrTrxStuck, // a transmit or a reply that never ended
    HbrTrxAborted,
} HbrTrx;

static uint8_t hbr_rx_buf[256];
static uint32_t hbr_stuck;

uint32_t hbr_hal_stuck_count(void) {
    return hbr_stuck;
}

static HbrTrx hbr_trx(HbrHal* hal, HbrTx kind, uint32_t fwt_fc) {
    furi_hal_nfc_trx_reset();
    // the gap after the last frame (FDT poll) runs on the block TX timer
    uint32_t t0 = furi_get_tick();
    while(furi_hal_nfc_timer_block_tx_is_running() && furi_get_tick() - t0 < HBR_STEP_MS) {
        if(furi_hal_nfc_poller_wait_event(HBR_SLICE_MS) & FuriHalNfcEventTimerBlockTxExpired) break;
    }

    FuriHalNfcError error;
    if(kind == HbrTxWupa) {
        // the firmware sends WUPA for SensReq and REQA for AllReq (furi_hal maps AllReq to TRANSMIT_REQA; seen on a
        // sniffer), as its own 14443-3A poller does
        error = furi_hal_nfc_iso14443a_poller_trx_short_frame(FuriHalNfcaShortFrameSensReq);
    } else if(kind == HbrTxSdd) {
        error = furi_hal_nfc_iso14443a_tx_sdd_frame(
            bit_buffer_get_data(hal->tx), bit_buffer_get_size(hal->tx));
    } else {
        error = furi_hal_nfc_poller_tx(bit_buffer_get_data(hal->tx), bit_buffer_get_size(hal->tx));
    }
    if(error != FuriHalNfcErrorNone) return HbrTrxBroken;

    HbrTrx ret = HbrTrxStuck;
    bool tx_done = false, rx_started = false;
    uint32_t deadline = furi_get_tick() + HBR_STEP_MS;
    while(true) {
        int32_t left = (int32_t)(deadline - furi_get_tick());
        if(left <= 0) break;
        if(hal->abort && *hal->abort) {
            ret = HbrTrxAborted;
            break;
        }
        FuriHalNfcEvent event = furi_hal_nfc_poller_wait_event(MIN((uint32_t)left, HBR_SLICE_MS));
        if((event & FuriHalNfcEventTxEnd) && !tx_done) {
            tx_done = true;
            if(fwt_fc) furi_hal_nfc_timer_fwt_start(fwt_fc);
            furi_hal_nfc_timer_block_tx_start_us(ISO14443_3A_POLL_POLL_MIN_US);
            // the FWT timer ends the wait first; this only guards against it
            deadline = furi_get_tick() + fwt_fc / HBR_FC_PER_MS + HBR_STEP_MS;
        }
        if((event & FuriHalNfcEventRxStart) && tx_done && !rx_started) {
            rx_started = true;
            furi_hal_nfc_timer_block_tx_stop();
            furi_hal_nfc_timer_fwt_stop();
            deadline = furi_get_tick() + HBR_STEP_MS;
        }
        if(event & FuriHalNfcEventRxEnd) {
            furi_hal_nfc_timer_block_tx_start(ISO14443_3A_FDT_POLL_FC);
            furi_hal_nfc_timer_fwt_stop();
            ret = HbrTrxOk;
            break;
        }
        if((event & FuriHalNfcEventTimerFwtExpired) && tx_done && !rx_started) {
            ret = HbrTrxNoAnswer;
            break;
        }
    }
    if(ret != HbrTrxOk) {
        furi_hal_nfc_timer_fwt_stop();
        if(ret != HbrTrxNoAnswer) furi_hal_nfc_trx_reset();
        if(ret == HbrTrxStuck) hbr_stuck++;
        return ret;
    }

    size_t bits = 0;
    error = furi_hal_nfc_poller_rx(hbr_rx_buf, sizeof(hbr_rx_buf), &bits);
    if(bits > sizeof(hbr_rx_buf) * 8) bits = sizeof(hbr_rx_buf) * 8;
    bit_buffer_copy_bits(hal->rx, hbr_rx_buf, bits); // also what came of a broken frame, for the trace
    return error == FuriHalNfcErrorNone ? HbrTrxOk : HbrTrxBroken;
}

// A CRC-framed frame: CRC appended to hal->tx, the answer's CRC checked and trimmed from hal->rx.
static bool hbr_xchg(HbrHal* hal, uint32_t fwt_fc) {
    if(hal->abort && *hal->abort) return hbr_fail(hal, HbrErrAborted);
    iso14443_crc_append(Iso14443CrcTypeA, hal->tx);
    hbr_log(hal, HbrTraceReader, hal->tx, 0);
    bit_buffer_reset(hal->rx);
    HbrTrx trx = hbr_trx(hal, HbrTxFrame, fwt_fc);
    if(trx == HbrTrxAborted) return hbr_fail(hal, HbrErrAborted);
    if(trx == HbrTrxNoAnswer) {
        hbr_note(hal, "no answer");
        return hbr_fail(hal, HbrErrTimeout);
    }
    if(trx == HbrTrxStuck) {
        hbr_note(hal, "frame never ended");
        return hbr_fail(hal, HbrErrTimeout);
    }
    if(trx != HbrTrxOk) {
        if(bit_buffer_get_size(hal->rx) > 0) hbr_log(hal, HbrTraceCard, hal->rx, 0);
        hbr_note(hal, "broken frame");
        return hbr_fail(hal, HbrErrFrame);
    }
    bool crc_ok = iso14443_crc_check(Iso14443CrcTypeA, hal->rx);
    hbr_log(hal, HbrTraceCard, hal->rx, crc_ok ? 0 : HBR_TRACE_CRC_BAD);
    if(!crc_ok) return hbr_fail(hal, HbrErrCrc);
    iso14443_crc_trim(hal->rx);
    return true;
}

// ----------------------------------------------------------------------------------------------------------------
// Activation at 106 kbit/s
// ----------------------------------------------------------------------------------------------------------------

bool hbr_hal_activate(HbrHal* hal, HbrActivation* act, bool* card_seen) {
    memset(act, 0, sizeof(*act));
    *card_seen = false;
    const uint32_t fwt = ISO14443_3A_FDT_LISTEN_FC;

    // WUPA rather than REQA: it also wakes a card the last read left in HALT
    hal->t_start = hbr_cycles();
    const uint8_t wupa = 0x52;
    hbr_trace_add_frame(hal->trace, HbrTraceReader, &wupa, 1, 7, 0);
    if(hbr_trx(hal, HbrTxWupa, fwt) != HbrTrxOk) return false;
    *card_seen = true;
    hbr_log(hal, HbrTraceCard, hal->rx, 0);
    if(bit_buffer_get_size_bytes(hal->rx) != 2) return false;
    act->atqa[0] = bit_buffer_get_byte(hal->rx, 0);
    act->atqa[1] = bit_buffer_get_byte(hal->rx, 1);

    // cascade levels 1..3 (SEL 93 / 95 / 97): anticollision (no CRC), then SELECT, until the SAK says complete
    for(uint8_t level = 0; level < 3; level++) {
        const uint8_t sel = (uint8_t)(0x93 + 2 * level);
        bit_buffer_reset(hal->tx);
        bit_buffer_append_byte(hal->tx, sel);
        bit_buffer_append_byte(hal->tx, 0x20);
        hbr_log(hal, HbrTraceReader, hal->tx, 0);
        if(hbr_trx(hal, HbrTxSdd, fwt) != HbrTrxOk) return false;
        hbr_log(hal, HbrTraceCard, hal->rx, 0);
        if(bit_buffer_get_size_bytes(hal->rx) != 5) return false;
        uint8_t nfcid[5];
        bit_buffer_write_bytes(hal->rx, nfcid, sizeof(nfcid));
        if((nfcid[0] ^ nfcid[1] ^ nfcid[2] ^ nfcid[3]) != nfcid[4]) return false;

        bit_buffer_reset(hal->tx);
        bit_buffer_append_byte(hal->tx, sel);
        bit_buffer_append_byte(hal->tx, 0x70);
        bit_buffer_append_bytes(hal->tx, nfcid, sizeof(nfcid));
        if(!hbr_xchg(hal, fwt)) return false;
        if(bit_buffer_get_size_bytes(hal->rx) != 1) return false;
        uint8_t sak = bit_buffer_get_byte(hal->rx, 0);

        if(sak & 0x04) {
            // UID not complete: this level held the cascade tag and 3 UID bytes
            if((size_t)act->uid_len + 3 <= sizeof(act->uid)) {
                memcpy(&act->uid[act->uid_len], &nfcid[1], 3);
                act->uid_len += 3;
            }
        } else {
            if((size_t)act->uid_len + 4 <= sizeof(act->uid)) {
                memcpy(&act->uid[act->uid_len], &nfcid[0], 4);
                act->uid_len += 4;
            }
            act->sak = sak;
            return true;
        }
    }
    return false;
}

// ----------------------------------------------------------------------------------------------------------------
// RATS / ATS
// ----------------------------------------------------------------------------------------------------------------

static void hbr_wait_us(uint32_t us) {
    if(us > 2000)
        furi_delay_ms((us + 999) / 1000);
    else if(us)
        furi_delay_us(us);
}

bool hbr_hal_rats(HbrHal* hal) {
    // FSDI 8: the reader takes frames up to 256 bytes; CID 0
    bit_buffer_reset(hal->tx);
    bit_buffer_append_byte(hal->tx, 0xE0);
    bit_buffer_append_byte(hal->tx, 0x80);
    if(!hbr_xchg(hal, HBR_FWT_FLOOR_FC)) return false;

    size_t n = bit_buffer_get_size_bytes(hal->rx);
    if(n < 1 || n > sizeof(hal->ats)) return hbr_protocol_error(hal);
    bit_buffer_write_bytes(hal->rx, hal->ats, n);
    hal->ats_len = (uint8_t)n;

    // T0: FSCI, then which of TA1 / TB1 / TC1 follow. Absent bytes take the ISO defaults.
    uint8_t fsci = 2, fwi = 4, sfgi = 0, ta1 = 0;
    if(n >= 2) {
        uint8_t t0 = hal->ats[1];
        fsci = t0 & 0x0F;
        size_t i = 2;
        if((t0 & 0x10) && i < n) ta1 = hal->ats[i++];
        if((t0 & 0x20) && i < n) {
            fwi = hal->ats[i] >> 4;
            sfgi = hal->ats[i] & 0x0F;
        }
    }
    static const uint16_t fsc_table[] = {16, 24, 32, 40, 48, 64, 96, 128, 256};
    hal->fsc = (fsci < COUNT_OF(fsc_table)) ? fsc_table[fsci] : 256;
    if(fwi > 14) fwi = 4;
    hal->fwt_fc = (4096u << fwi) + HBR_DELTA_FWT_FC;
    if(hal->fwt_fc < HBR_FWT_FLOOR_FC) hal->fwt_fc = HBR_FWT_FLOOR_FC;
    hal->blocknum = 0;
    hbr_note(hal, "FSC %u FWI %u TA1 %02X", hal->fsc, fwi, ta1);

    // SFGT: the card may need this long after its ATS before the next frame
    if(sfgi >= 1 && sfgi <= 14) {
        uint32_t sfgt_fc = (4096u << sfgi) + (384u << sfgi);
        hal->sfgt_us = (uint32_t)(((uint64_t)sfgt_fc * 1000u) / 13560u);
        hbr_wait_us(hal->sfgt_us);
    }
    return true;
}

// ----------------------------------------------------------------------------------------------------------------
// PPS and the ST25R3916 at 212 / 424 / 848 kbit/s
// ----------------------------------------------------------------------------------------------------------------
//
// The ST25R3916 is a full NFC-A reader up to 848 kbit/s, and every register needed is reachable from an app through
// the firmware's exported st25r3916_* helpers. Two things matter:
//  - Above 106 kbit/s the reader's own frames are AM modulated, not the on/off keying of 106: MODE.tr_am, the
//    TX_DRIVER AM depth and AUX_MOD (AM through the regulator) change, and the over/undershoot protection that
//    shapes 106 pauses is off. Left at 106 OOK the card never decodes a frame at the new rate.
//  - The receiver gets the AM channel filter and correlator settings (ST's RFAL values for polling NFC-A at the high
//    rates). The subcarrier is fc/16 at every rate, so one set serves 212, 424 and 848; only BIT_RATE differs.
// These run in the NFC worker's callback, which already holds the NFC SPI bus: the helpers write it directly.
// Acquiring the bus again here would deadlock (the mutex is not recursive).

static const uint8_t hbr_regs[HBR_HAL_SAVED_REGS] = {
    ST25R3916_REG_BIT_RATE,
    ST25R3916_REG_MODE,
    ST25R3916_REG_TX_DRIVER,
    ST25R3916_REG_AUX_MOD,
    ST25R3916_REG_OVERSHOOT_CONF1,
    ST25R3916_REG_OVERSHOOT_CONF2,
    ST25R3916_REG_UNDERSHOOT_CONF1,
    ST25R3916_REG_UNDERSHOOT_CONF2,
    ST25R3916_REG_RX_CONF1,
    ST25R3916_REG_RX_CONF2,
    ST25R3916_REG_RX_CONF3,
    ST25R3916_REG_RX_CONF4,
    ST25R3916_REG_CORR_CONF1,
    ST25R3916_REG_CORR_CONF2,
};

static void hbr_save_regs(HbrHal* hal) {
    const FuriHalSpiBusHandle* h = &furi_hal_spi_bus_handle_nfc;
    for(size_t i = 0; i < HBR_HAL_SAVED_REGS; i++) {
        st25r3916_read_reg(h, hbr_regs[i], &hal->saved_regs[i]);
    }
    hal->regs_saved = true;
}

static void hbr_apply_rate(uint8_t di) {
    static const uint8_t bit_rate[4] = {
        ST25R3916_REG_BIT_RATE_txrate_106 | ST25R3916_REG_BIT_RATE_rxrate_106,
        ST25R3916_REG_BIT_RATE_txrate_212 | ST25R3916_REG_BIT_RATE_rxrate_212,
        ST25R3916_REG_BIT_RATE_txrate_424 | ST25R3916_REG_BIT_RATE_rxrate_424,
        ST25R3916_REG_BIT_RATE_txrate_848 | ST25R3916_REG_BIT_RATE_rxrate_848,
    };
    const FuriHalSpiBusHandle* h = &furi_hal_spi_bus_handle_nfc;
    st25r3916_change_reg_bits(
        h,
        ST25R3916_REG_BIT_RATE,
        ST25R3916_REG_BIT_RATE_txrate_mask | ST25R3916_REG_BIT_RATE_rxrate_mask,
        bit_rate[di & 3]);
    // TX: AM, 40% depth, through the regulator; no 106 pause shaping
    st25r3916_change_reg_bits(h, ST25R3916_REG_MODE, ST25R3916_REG_MODE_tr_am, ST25R3916_REG_MODE_tr_am_am);
    st25r3916_change_reg_bits(
        h,
        ST25R3916_REG_TX_DRIVER,
        ST25R3916_REG_TX_DRIVER_am_mod_mask,
        ST25R3916_REG_TX_DRIVER_am_mod_40percent);
    st25r3916_change_reg_bits(
        h, ST25R3916_REG_AUX_MOD, ST25R3916_REG_AUX_MOD_dis_reg_am | ST25R3916_REG_AUX_MOD_res_am, 0x00);
    st25r3916_write_reg(h, ST25R3916_REG_OVERSHOOT_CONF1, 0x00);
    st25r3916_write_reg(h, ST25R3916_REG_OVERSHOOT_CONF2, 0x00);
    st25r3916_write_reg(h, ST25R3916_REG_UNDERSHOOT_CONF1, 0x00);
    st25r3916_write_reg(h, ST25R3916_REG_UNDERSHOOT_CONF2, 0x00);
    // RX: AM channel, 2 MHz low pass, 80 kHz zero; AGC with squelch ratio 6/3; BPSK correlator settings
    st25r3916_write_reg(h, ST25R3916_REG_RX_CONF1, 0x42);
    st25r3916_write_reg(h, ST25R3916_REG_RX_CONF2, 0x3D);
    st25r3916_write_reg(h, ST25R3916_REG_RX_CONF3, 0x00);
    st25r3916_write_reg(h, ST25R3916_REG_RX_CONF4, 0x00);
    st25r3916_write_reg(h, ST25R3916_REG_CORR_CONF1, 0x44);
    st25r3916_write_reg(h, ST25R3916_REG_CORR_CONF2, 0x00);
}

void hbr_hal_restore(HbrHal* hal) {
    if(!hal->regs_saved) return;
    const FuriHalSpiBusHandle* h = &furi_hal_spi_bus_handle_nfc;
    for(size_t i = 0; i < HBR_HAL_SAVED_REGS; i++) {
        st25r3916_write_reg(h, hbr_regs[i], hal->saved_regs[i]);
    }
    hal->regs_saved = false;
    hal->rate = 106;
}

HbrPpsResult hbr_hal_pps(HbrHal* hal, uint16_t kbps) {
    uint8_t di = (kbps == 848) ? 3 : (kbps == 424) ? 2 : (kbps == 212) ? 1 : 0;
    if(di == 0) return HbrPpsActive;

    // TA1 follows T0 when T0 bit 5 is set. TA1: bits 7..5 = card-to-reader 848 / 424 / 212, bits 3..1 =
    // reader-to-card 848 / 424 / 212, bit 8 = "same rate both ways only", which this symmetric PPS always is.
    if(hal->ats_len < 3 || !(hal->ats[1] & 0x10)) return HbrPpsNoTa1;
    uint8_t ta1 = hal->ats[2];
    uint8_t ds = (uint8_t)(0x10 << (di - 1));
    uint8_t dr = (uint8_t)(0x01 << (di - 1));
    if(!(ta1 & ds) || !(ta1 & dr)) return HbrPpsNotAdvertised;

    // PPSS (PPS, CID 0), PPS0 (PPS1 follows), PPS1 = DSI << 2 | DRI; sent and answered at 106
    bit_buffer_reset(hal->tx);
    bit_buffer_append_byte(hal->tx, 0xD0);
    bit_buffer_append_byte(hal->tx, 0x11);
    bit_buffer_append_byte(hal->tx, (uint8_t)((di << 2) | di));
    if(!hbr_xchg(hal, HBR_FWT_FLOOR_FC)) return HbrPpsNoAnswer;
    if(bit_buffer_get_size_bytes(hal->rx) < 1 || bit_buffer_get_byte(hal->rx, 0) != 0xD0) {
        hbr_protocol_error(hal);
        return HbrPpsBadAnswer;
    }

    // The card switches after its answer: the reader follows now, before the first frame at the new rate
    hbr_save_regs(hal);
    hbr_apply_rate(di);
    hal->rate = kbps;
    hbr_note(hal, "reader at %u kbit/s", kbps);
    return HbrPpsActive;
}

// ----------------------------------------------------------------------------------------------------------------
// ISO 14443-4 blocks
// ----------------------------------------------------------------------------------------------------------------

// Sends hal->tx as one block; returns the card's next non-WTX block in hal->rx (S(WTX) requests are granted).
static bool hbr_block(HbrHal* hal) {
    uint32_t fwt = hal->fwt_fc;
    for(int i = 0; i < 64; i++) {
        if(!hbr_xchg(hal, fwt)) return false;
        size_t n = bit_buffer_get_size_bytes(hal->rx);
        if(n == 0) return hbr_protocol_error(hal);
        uint8_t pcb = bit_buffer_get_byte(hal->rx, 0);
        if((pcb & 0xF7) != PCB_S_WTX) return true;
        if(n < 2) return hbr_protocol_error(hal);
        uint8_t wtxm = bit_buffer_get_byte(hal->rx, 1) & 0x3F;
        if(wtxm == 0) wtxm = 1;
        if(wtxm > 59) wtxm = 59;
        bit_buffer_reset(hal->tx);
        bit_buffer_append_byte(hal->tx, PCB_S_WTX);
        bit_buffer_append_byte(hal->tx, wtxm);
        uint64_t wait = (uint64_t)hal->fwt_fc * wtxm;
        fwt = (uint32_t)(wait > HBR_FWT_MAX_FC ? HBR_FWT_MAX_FC : wait);
    }
    return hbr_protocol_error(hal);
}

bool hbr_hal_apdu(
    HbrHal* hal,
    const uint8_t* apdu,
    size_t len,
    uint8_t* resp,
    size_t resp_max,
    size_t* resp_len) {
    *resp_len = 0;
    size_t max_inf = (hal->fsc > 4) ? (size_t)(hal->fsc - 3) : 1; // PCB and CRC take 3 bytes

    // out: I-blocks, every one but the last with the chaining bit and acknowledged by R(ACK)
    size_t sent = 0;
    for(;;) {
        size_t chunk = len - sent;
        bool more = chunk > max_inf;
        if(more) chunk = max_inf;
        bit_buffer_reset(hal->tx);
        bit_buffer_append_byte(hal->tx, (uint8_t)(PCB_I | hal->blocknum | (more ? PCB_I_CHAIN : 0)));
        bit_buffer_append_bytes(hal->tx, apdu + sent, chunk);
        if(!hbr_block(hal)) return false;
        sent += chunk;
        if(!more) break;
        uint8_t pcb = bit_buffer_get_byte(hal->rx, 0);
        if((pcb & 0xF6) != PCB_R_ACK || (pcb & PCB_BLOCKNUM) != hal->blocknum) {
            return hbr_protocol_error(hal);
        }
        hal->blocknum ^= 1;
    }

    // in: I-blocks with our block number; a chained one is acknowledged by R(ACK) with the next block number
    for(;;) {
        uint8_t pcb = bit_buffer_get_byte(hal->rx, 0);
        if((pcb & 0xE2) != PCB_I || (pcb & PCB_BLOCKNUM) != hal->blocknum) {
            return hbr_protocol_error(hal);
        }
        size_t n = bit_buffer_get_size_bytes(hal->rx) - 1;
        if(*resp_len + n > resp_max) {
            hbr_note(hal, "response too long");
            return hbr_fail(hal, HbrErrOverflow);
        }
        bit_buffer_write_bytes_mid(hal->rx, resp + *resp_len, 1, n);
        *resp_len += n;
        hal->blocknum ^= 1;
        if(!(pcb & PCB_I_CHAIN)) return true;
        bit_buffer_reset(hal->tx);
        bit_buffer_append_byte(hal->tx, (uint8_t)(PCB_R_ACK | hal->blocknum));
        if(!hbr_block(hal)) return false;
    }
}

bool hbr_hal_deselect(HbrHal* hal) {
    bit_buffer_reset(hal->tx);
    bit_buffer_append_byte(hal->tx, PCB_S_DESELECT);
    if(!hbr_xchg(hal, hal->fwt_fc)) return false;
    if(bit_buffer_get_size_bytes(hal->rx) < 1 ||
       (bit_buffer_get_byte(hal->rx, 0) & 0xF7) != PCB_S_DESELECT) {
        return hbr_protocol_error(hal);
    }
    return true;
}

uint32_t hbr_hal_elapsed_us(const HbrHal* hal) {
    uint32_t per_us = furi_hal_cortex_instructions_per_microsecond();
    if(per_us == 0) per_us = 64;
    return (hal->t_last - hal->t_start) / per_us;
}
