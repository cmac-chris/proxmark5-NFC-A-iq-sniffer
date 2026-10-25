// SPDX-License-Identifier: GPL-3.0-or-later
#include "hbr_workload.h"

#include <string.h>

#define HBR_RESP_MAX 300 // one response, chained frames reassembled
#define HBR_MAX_APPS 28
#define HBR_MAX_FILES 32
#define HBR_MAX_CONT 600 // continuation frames of one command (a 32 KB file is ~560 of 59 bytes)

#define HBR_SW_OK 0x9100 // DESFire: OK
#define HBR_SW_MORE 0x91AF // DESFire: more data follows
#define HBR_ISO_OK 0x9000

typedef struct {
    HbrHal* hal;
    uint8_t* resp; // HBR_RESP_MAX
    uint8_t* data; // the rest of the scratch: data gathered over continuation frames
    size_t data_max;
} HbrWork;

static uint16_t hbr_sw(const uint8_t* resp, size_t len) {
    return len >= 2 ? (uint16_t)((resp[len - 2] << 8) | resp[len - 1]) : 0;
}

// A DESFire command wrapped in ISO 7816 (90 INS 00 00 [Lc data] 00), with its 91 AF continuations; the data
// (without status words) gathered into w->data as far as it fits, the last status word in *sw.
static bool hbr_df(HbrWork* w, uint8_t ins, const uint8_t* arg, size_t arg_len, size_t* data_len, uint16_t* sw) {
    uint8_t apdu[24];
    size_t n = 0;
    apdu[n++] = 0x90;
    apdu[n++] = ins;
    apdu[n++] = 0x00;
    apdu[n++] = 0x00;
    if(arg_len) {
        apdu[n++] = (uint8_t)arg_len;
        memcpy(&apdu[n], arg, arg_len);
        n += arg_len;
    }
    apdu[n++] = 0x00;

    *data_len = 0;
    *sw = 0;
    for(int frames = 0; frames < HBR_MAX_CONT; frames++) {
        size_t rlen = 0;
        if(!hbr_hal_apdu(w->hal, apdu, n, w->resp, HBR_RESP_MAX, &rlen)) return false;
        *sw = hbr_sw(w->resp, rlen);
        if(rlen < 2) return true; // no status word: an answer all the same
        size_t d = rlen - 2;
        if(*data_len + d > w->data_max) d = w->data_max - *data_len;
        memcpy(w->data + *data_len, w->resp, d);
        *data_len += d;
        if(*sw != HBR_SW_MORE) return true;
        // more data: 90 AF 00 00 00
        apdu[1] = 0xAF;
        apdu[4] = 0x00;
        n = 5;
    }
    return true;
}

// An ISO 7816 command; the response (status word included) in w->resp
static bool hbr_iso(HbrWork* w, const uint8_t* apdu, size_t len, size_t* rlen) {
    return hbr_hal_apdu(w->hal, apdu, len, w->resp, HBR_RESP_MAX, rlen);
}

// ISO 7816 SELECT by DF name (00 A4 04 00 Lc name 00); returns its status word in *sw
static bool hbr_select_name(HbrWork* w, const uint8_t* name, size_t name_len, uint16_t* sw) {
    uint8_t apdu[24];
    size_t n = 0;
    apdu[n++] = 0x00;
    apdu[n++] = 0xA4;
    apdu[n++] = 0x04;
    apdu[n++] = 0x00;
    apdu[n++] = (uint8_t)name_len;
    memcpy(&apdu[n], name, name_len);
    n += name_len;
    apdu[n++] = 0x00;
    size_t rlen = 0;
    if(!hbr_iso(w, apdu, n, &rlen)) return false;
    *sw = hbr_sw(w->resp, rlen);
    return true;
}

// ----------------------------------------------------------------------------------------------------------------
// MIFARE DESFire
// ----------------------------------------------------------------------------------------------------------------

// GetKeySettings, then GetKeyVersion of each key (the key count is in the low nibble of the second byte)
static bool hbr_df_keys(HbrWork* w) {
    size_t len;
    uint16_t sw;
    if(!hbr_df(w, 0x45, NULL, 0, &len, &sw)) return false;
    uint8_t keys = (sw == HBR_SW_OK && len >= 2) ? (w->data[1] & 0x0F) : 1;
    if(keys > 14) keys = 14;
    for(uint8_t k = 0; k < keys; k++) {
        if(!hbr_df(w, 0x64, &k, 1, &len, &sw)) return false;
    }
    return true;
}

// The contents of a file that is free to read: ReadData / ReadRecords with offset 0 and length 0 (everything),
// GetValue for a value file
static bool hbr_df_read_file(HbrWork* w, uint8_t fid, uint8_t type) {
    size_t len;
    uint16_t sw;
    const uint8_t all[7] = {fid, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    switch(type) {
    case 0x00: // standard data
    case 0x01: // backup data
        return hbr_df(w, 0xBD, all, sizeof(all), &len, &sw);
    case 0x03: // linear record
    case 0x04: // cyclic record
        return hbr_df(w, 0xBB, all, sizeof(all), &len, &sw);
    case 0x02: // value
        return hbr_df(w, 0x6C, &fid, 1, &len, &sw);
    default:
        return true;
    }
}

static bool hbr_desfire_walk(HbrWork* w) {
    size_t len;
    uint16_t sw;
    uint8_t aids[HBR_MAX_APPS * 3];
    uint8_t fids[HBR_MAX_FILES];

    // card level: keys, free memory, originality signature, application IDs, DF names
    if(!hbr_df_keys(w)) return false;
    if(!hbr_df(w, 0x6E, NULL, 0, &len, &sw)) return false;
    const uint8_t sig_addr = 0x00;
    if(!hbr_df(w, 0x3C, &sig_addr, 1, &len, &sw)) return false;
    if(!hbr_df(w, 0x6A, NULL, 0, &len, &sw)) return false;
    size_t aids_len = (sw == HBR_SW_OK) ? len - (len % 3) : 0;
    if(aids_len > sizeof(aids)) aids_len = sizeof(aids);
    memcpy(aids, w->data, aids_len);
    if(!hbr_df(w, 0x6D, NULL, 0, &len, &sw)) return false;

    // each application: select, keys, file IDs; each file: settings, and its contents when it is free to read
    for(size_t a = 0; a + 3 <= aids_len; a += 3) {
        if(!hbr_df(w, 0x5A, &aids[a], 3, &len, &sw)) return false;
        if(sw != HBR_SW_OK) continue;
        if(!hbr_df_keys(w)) return false;
        if(!hbr_df(w, 0x6F, NULL, 0, &len, &sw)) return false;
        if(sw != HBR_SW_OK) continue;
        size_t fids_len = len < sizeof(fids) ? len : sizeof(fids);
        memcpy(fids, w->data, fids_len);
        for(size_t f = 0; f < fids_len; f++) {
            if(!hbr_df(w, 0xF5, &fids[f], 1, &len, &sw)) return false;
            // settings: type, communication, access rights (read & write | change, read | write), ...
            if(sw != HBR_SW_OK || len < 4) continue;
            uint8_t type = w->data[0];
            bool free_read = (w->data[3] >> 4) == 0x0E || (w->data[2] >> 4) == 0x0E;
            if(free_read && !hbr_df_read_file(w, fids[f], type)) return false;
        }
    }

    // back to the card level
    const uint8_t picc[3] = {0x00, 0x00, 0x00};
    return hbr_df(w, 0x5A, picc, sizeof(picc), &len, &sw);
}

// ----------------------------------------------------------------------------------------------------------------
// Other ISO 14443-4 cards
// ----------------------------------------------------------------------------------------------------------------

// NFC Forum type 4 tag, its application selected: the capability container, then the NDEF file when the CC says
// it is free to read
static bool hbr_t4t_read(HbrWork* w) {
    size_t rlen;
    const uint8_t sel_cc[] = {0x00, 0xA4, 0x00, 0x0C, 0x02, 0xE1, 0x03};
    if(!hbr_iso(w, sel_cc, sizeof(sel_cc), &rlen)) return false;
    if(hbr_sw(w->resp, rlen) != HBR_ISO_OK) return true;
    const uint8_t read_cc[] = {0x00, 0xB0, 0x00, 0x00, 0x0F};
    if(!hbr_iso(w, read_cc, sizeof(read_cc), &rlen)) return false;
    // CC: length (2), version (1), MLe (2), MLc (2), NDEF file control TLV: 04 06 id (2) size (2) read write
    if(hbr_sw(w->resp, rlen) != HBR_ISO_OK || rlen < 15 + 2 || w->resp[7] != 0x04) return true;
    uint16_t mle = (uint16_t)((w->resp[3] << 8) | w->resp[4]);
    uint8_t fid_hi = w->resp[9], fid_lo = w->resp[10];
    if(w->resp[13] != 0x00) return true; // NDEF read access not free

    const uint8_t sel_ndef[] = {0x00, 0xA4, 0x00, 0x0C, 0x02, fid_hi, fid_lo};
    if(!hbr_iso(w, sel_ndef, sizeof(sel_ndef), &rlen)) return false;
    if(hbr_sw(w->resp, rlen) != HBR_ISO_OK) return true;
    const uint8_t read_nlen[] = {0x00, 0xB0, 0x00, 0x00, 0x02};
    if(!hbr_iso(w, read_nlen, sizeof(read_nlen), &rlen)) return false;
    if(hbr_sw(w->resp, rlen) != HBR_ISO_OK || rlen < 4) return true;
    uint32_t total = 2u + (uint32_t)((w->resp[0] << 8) | w->resp[1]);

    // READ BINARY in pieces of MLe at most (and within the reader's 256-byte frames)
    uint16_t piece = (mle >= 1 && mle < 240) ? mle : 240;
    for(uint32_t off = 2; off < total && off < 0x8000;) {
        uint32_t want = total - off;
        if(want > piece) want = piece;
        const uint8_t rd[] = {
            0x00, 0xB0, (uint8_t)(off >> 8), (uint8_t)(off & 0xFF), (uint8_t)want};
        if(!hbr_iso(w, rd, sizeof(rd), &rlen)) return false;
        if(hbr_sw(w->resp, rlen) != HBR_ISO_OK || rlen < 3) break;
        off += (uint32_t)(rlen - 2);
    }
    return true;
}

static bool hbr_iso_probes(HbrWork* w) {
    static const uint8_t ppse[] = "2PAY.SYS.DDF01"; // payment directory
    static const uint8_t ndef[] = {0xD2, 0x76, 0x00, 0x00, 0x85, 0x01, 0x01}; // NFC Forum type 4 tag
    static const uint8_t seos[] = {0xA0, 0x00, 0x00, 0x04, 0x40, 0x00, 0x01, 0x01, 0x00, 0x01}; // HID Seos
    static const uint8_t emrtd[] = {0xA0, 0x00, 0x00, 0x02, 0x47, 0x10, 0x01}; // travel documents
    uint16_t sw;
    if(!hbr_select_name(w, ppse, sizeof(ppse) - 1, &sw)) return false;
    if(!hbr_select_name(w, ndef, sizeof(ndef), &sw)) return false;
    if(sw == HBR_ISO_OK && !hbr_t4t_read(w)) return false;
    if(!hbr_select_name(w, seos, sizeof(seos), &sw)) return false;
    return hbr_select_name(w, emrtd, sizeof(emrtd), &sw);
}

bool hbr_workload_run(HbrHal* hal, uint8_t* scratch) {
    HbrWork w = {
        .hal = hal,
        .resp = scratch,
        .data = scratch + HBR_RESP_MAX,
        .data_max = HBR_WORKLOAD_SCRATCH - HBR_RESP_MAX,
    };

    // GetVersion: answered by DESFire, MIFARE Plus EV1+ and NTAG 4xx; other cards return an error status
    size_t len;
    uint16_t sw;
    if(!hbr_df(&w, 0x60, NULL, 0, &len, &sw)) return false;
    // NXP (04), DESFire (01, or 81) or DESFire Light (08)
    bool desfire = (sw == HBR_SW_OK) && len >= 7 && w.data[0] == 0x04 &&
                   ((w.data[1] & 0x7F) == 0x01 || w.data[1] == 0x08);
    hbr_trace_add_note(hal->trace, desfire ? "DESFire walk" : "ISO 7816 probes");
    return desfire ? hbr_desfire_walk(&w) : hbr_iso_probes(&w);
}
