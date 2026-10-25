# 14A HBR Tester (Flipper Zero)

A Flipper Zero app that reads ISO 14443-4 cards at **106, 212, 424 or 848 kbit/s** and shows:

- **PASS / FAIL**
- the number of **frames** on the air
- the **time**, from the first frame to the card's last frame
- the **full frame trace** the Flipper kept: every frame it sent and received, CRC included, nothing truncated

It is made for testing the Proxmark5's `hf 14a sniff`. Start the sniff, read a card with this app, and compare the sniffer's trace with the Flipper's frame by frame: the Flipper's trace is what was on the air.

Nothing on the Flipper has to change: the app works on stock firmware and the common custom ones (see
[Firmware](#firmware)). To install the prebuilt `.fap`, see
[Test with a Flipper Zero](../doc/md/PM5_Start_Here/PM5-14A-SNIFF-SETUP.md#test-with-a-flipper-zero).

## Use

1. **Speed** (left / right): 106 / 212 / 424 / 848 kbit/s, or **Cycle**: every read takes the next rate,
   106 → 212 → 424 → 848 → 106 …
2. Place the card, then **Read** for one read, or **Loop** to read again and again until Back. Back cancels a
   read while it waits for a card.
3. The result screen: PASS / FAIL, frames and time, the reason if it failed, the saved file, then the trace.
   Up / Down scroll, Left / Right page, OK jumps to the end and back.

Every read is saved to `/ext/apps_data/iso14443a_hbr_tester/read_NNNN_<speed>.txt`.

Put the card in place before pressing Read: the trace starts at the WUPA the card answered, while a sniffer that
was already running also sees the WUPAs sent before the card arrived.

## Loop

Loop gives a sniffer many activations, PPS changes and thousands of frames in one capture. Every read is a whole
session: the field goes off (at least 10 ms, so the card resets), back on, then WUPA through DESELECT as below.
With **Cycle** the reads step through the four rates, starting at 106, so the PPS changes on every read.

- The screen counts reads, FAILs, frames and time. The LED blinks cyan, and red after a failed read.
- Taking the card away pauses the loop; it carries on when the card is back.
- It stops itself after 10 failed reads in a row, or when the SD card can't be written.
- Every read goes to `/ext/apps_data/iso14443a_hbr_tester/loop_NNNN_<speed>.txt` (`<speed>` is the rate or
  `cycle`): one line per read (number, rate, PASS / FAIL and why, frames, time), then its frames; the totals at
  the end. A read that ends just as Back is pressed still counts; one that Back cuts short is in the file (marked,
  not counted), as a sniffer saw it too.
- When it stops: the totals, the file, and the trace of the last finished read.

A sniffer's buffer fills up during a long loop: the Proxmark5's holds about 420 KB of trace, on the order of
10,000 frames. Its timestamps (in carrier cycles) also wrap after 5 min 17 s.

Stacked tight (card on the Proxmark, Flipper on top), an idle Proxmark can block the Flipper from the card: start
the sniff before the reads. A sniff that ends mid-read (its button, a full buffer) can make that read FAIL.

## What a read does

- **Activation at 106 kbit/s**: WUPA, anticollision, SELECT, RATS (the Flipper takes frames up to 256 bytes).
- **212 / 424 / 848**: a PPS for that rate in both directions. The card's ATS must advertise the rate both ways
  (TA1); then the Flipper's NFC chip switches over.
- **Read-only exchanges at that rate**:
  - every card: GetVersion
  - MIFARE DESFire, the whole card: key settings and key versions, free memory, the originality signature,
    application IDs, DF names, and for each application its keys, file IDs and file settings, plus the contents of
    every file that is free to read (read or read & write access `E`)
  - other cards: SELECT of the payment directory, NFC Forum type 4 tag, Seos and travel document applications, and
    a type 4 tag's NDEF file when its capability container says it is free to read

  Nothing is written, and nothing is authenticated. A DESFire card whose files are all free to read gives the
  longest session.
- **S(DESELECT)** at the end.

**PASS** means every frame arrived intact (CRC, ISO 14443-4 framing) and the card answered everything up to the
DESELECT. A card refusing a command (an error status word) is still an answer: the test is about the link.

**FAIL** reasons: `Not 14443-4` (the card has no ISO 14443-4 layer), `Card: 106 only` (no TA1), `Card lacks 848`
(TA1 lacks that rate), `PPS not answered`, `Bad PPS answer`, `Timeout / CRC error / Bad frame / Protocol @ frame N`,
`Trace: out of memory` (the Flipper had no room left for the whole trace).

Every wait is bounded. The firmware's own exchange waits forever for a frame that started but never ends, and that
hung the app; here such a frame is cut off after 100 ms, the read FAILs with `Timeout` ("frame never ended" in the
trace), and the file counts them ("Frames that never ended", in a loop's totals). A sign of a disturbed link.

## Cards

Any ISO 14443-4 (ISO-DEP) card whose ATS advertises the chosen rate. MIFARE DESFire EV1 / EV2 / EV3 advertise up
to 848 kbit/s both ways. MIFARE Classic, Ultralight and NTAG 21x have no ISO 14443-4 layer and stay at 106.

## Firmware

A `.fap` is built against one firmware's API. The loader only accepts the same API **major** version, then checks
that every function the app imports exists. One build therefore covers every firmware with that major.

| API | File | Firmware it loads on |
|---|---|---|
| 88 | `iso14443a_hbr_tester_ofw-1.5.1_api88.2.fap` | Official 1.5.1, Unleashed unlshd-094 |
| 87 | `iso14443a_hbr_tester_mntm-012_api87.1.fap` | Momentum mntm-012, Official 1.4.3, Unleashed unlshd-086 |
| 86 | `iso14443a_hbr_tester_unlshd-082_api86.0.fap` | Unleashed unlshd-082 |

"Loads on" was checked against each firmware's API table: every function the app imports is there. Other
firmware with the same API major should take the same file.

The three files are in this folder, built from this source. To build them yourself: `./build_all.sh` ([ufbt](https://github.com/flipperdevices/flipperzero-ufbt), one SDK per
target, downloaded on its first build), or just `ufbt` for the SDK you have.

## How the high rates work

The Flipper's NFC chip, the ST25R3916, is a full NFC-A reader up to 848 kbit/s, and the firmware exports the
register helpers an app needs. After the card answers the PPS, `hbr_hal.c` saves and then rewrites these registers:

- **BIT_RATE**: transmit and receive at the new rate
- **Transmit**: AM modulation (MODE `tr_am`), 40% depth (TX_DRIVER), AM through the regulator (AUX_MOD), no
  overshoot / undershoot pause shaping. Above 106 kbit/s the reader's frames are AM, not 106's on/off keying; a
  card never decodes a frame sent the 106 way.
- **Receive**: RX_CONF1..4 = `42 3D 00 00`, CORR_CONF1..2 = `44 00` (ST's RFAL values for polling NFC-A at the high
  rates; one set serves 212, 424 and 848)

The writes run in the NFC worker's callback, which already holds the chip's SPI bus: taking it again would deadlock.
The saved values are written back at the end of every read.

## Limits

- The same rate both ways (no asymmetric PPS), no CID or NAD.
- The time comes from the Flipper's CPU clock around each exchange, so it includes a little software latency. It is
  not a sniffer's carrier-exact timing.
- The trace shows frames as the Flipper's NFC chip sent and received them.

## License

GPL-3.0-or-later.
