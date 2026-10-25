# Proxmark5 I/Q Sniffer for ISO 14443-A

A rebuilt `hf 14a sniff` for the Proxmark5: a sniffer-only FPGA image and matching ARM firmware that decode ISO/IEC 14443-A at 106, 212, 424 and 848 kbit/s, follow the PPS between rates, and time 106 kbit/s frames to within ±4 carrier cycles and 212, 424, and 848 frames to ~1-2 µs of accuracy.

## What's new

- Gateware for ISO 14443-A sniffing at all four supported bitrates (`pm5_hi_iso14443a.v`)
- ARM firmware decoders for 212 / 424 / 848 kbit/s, frame edge timing, and a fast loader for the new sniffer image


## Features

- Real I/Q passive sniffing: at 106 kbit/s, card responses are decoded by correlating with the 847.5 kHz subcarrier in phase and in quadrature, against a threshold that follows each response’s own level. At 212 - 848 kbit/s, card responses are decoded by BPSK demodulation against the phase of the response’s preamble.


- Significantly improved sniffing at 106 kbit/s: Full reader transaction traces succeed without errors in various placements, specifically ones where the stock sniffer struggled or did not work at all.


- 212, 424 and 848 kbit/s: a CRC-valid card PPS response switches both directions to the negotiated rate, and a REQA / WUPA switches back to 106 automatically.


- Antenna coupling tolerance: Sniffs work in every tested placement: the card held between the reader and the PM5; the PM5 between card and reader; the card in direct contact with the bare HF antenna; and the card and reader spaced up to 4.4 cm above the cover.


- Timing accuracy: 106 kbit/s frames are timed from their edges; card responses land within ±4 carrier cycles of the ISO values in every placement. 212 - 848 kbit/s frames are put on the same time base to about 1 - 2 µs. The sniffer was accurate enough to measure two cards’ own timing. Two different EV3s switched on their subcarriers 13 - 19 µs earlier at 212 - 848 kbit/s than they did at 106 kbit/s, and agreed within 0.6 µs.


- Full 10-bit ADC: `hf 14a sniff` now uses the entire dynamic range of the 10-bit ADC.


- Long traces: The client no longer cuts traces off at 64 KB; the PM5 now supports up to 420 KB long traces.


- Fast image loading: the sniffer image loads into FPGA SRAM in about 50 ms when `hf 14a sniff` starts. The FPGA's All-In-One image returns in about 200 ms after a sniff ends. This happens immediately after a sniff is completed, since the FPGA flash is never written to.


## Building and flashing

Flash the ARM fullimage; the bootrom and the FPGA flash are not touched. The sniffer image ships prebuilt (`fpga/fpga_pm5_sniff14a.bin`) and is embedded in the ARM image (PM5 `PLATFORM_SIZE` now defaults to 1024 KB; the full image is about 570 KB).

- To rebuild the image: `make -C fpga fpga_pm5_sniff14a` (oss-cad-suite: yosys, nextpnr-himbaechel, apicula).
- To build without it: `make PM5_SNIFF_IMAGE=` (sniffs at 106 kbit/s on the flash image, as before).



## Technical details


- Reader pauses are measured against the signal's own idle level on the full 10-bit samples—not against the ADC's rail—so weak and clipped pauses are still detected.


- Card responses at 106 kbit/s: I/Q correlation against the subcarrier. A card clipping the front end is followed by its dips instead.


- Card responses at 212 - 848 kbit/s: BPSK demodulation against the phase of the response's preamble.


- Timing: a 16-bit FPGA-to-ARM link carries an edge event beside every data byte (pause leads, response onsets, pause widths). At the higher rates, per-rate constants put frames on the air's time.


## Purpose

While developing a firmware fork for the Flipper Zero to increase read speed, I successfully enabled PPS rate negotiation and reading at rates exceeding 106 kbit/s (up to 848 kbit/s). To verify the Flipper's command handling and timing, I tried using the Proxmark3 to sniff the transaction, and found out that it did not support high-rate sniffing. The Proxmark5 hardware does, however, as this code and my testing prove. To my knowledge, this is the only open-source tool capable of sniffing 848 kbit/s ISO 14443-A frames.

## Testing

I used a Flipper Zero as the reader with a DESFire EV3 card. I used a range of placements to confirm the decoders don’t fall apart due to minor, or even major, differences in antenna coupling and noise environments. Anyone who wishes to test this sniffer code and also owns a Flipper Zero: rather than forking your firmware to enable the higher rates, I have packaged everything needed into a small Flipper application instead. The .fap and its source can be found in the `flipper_hbr_tester` folder of this repository.

30 of 32 test sniffs were completely error-free. At 106, 212, and 424 kbit/s, 8 out of 8 sniffs were error-free; at 848, 6 out of 8. The stock sniffer at 106 kbit/s passed only 2 out of 8 sniffs. The first failure at 848 was about 2 cm laterally away from the antenna (the coupling limit at that rate). The second failure was the card under the PM5 with the cover on, where the Flipper itself lost the card (everything on the air was still logged). This suggests that 848 is much less tolerant of poor coupling (the failed test was the card not centered on the antenna, but works in most placements, even 4.4 cm away from the antenna).

## Sample size

65,422 sniffed frames were compared one-by-one with the reader's own record of the same exchanges. They were all byte identical. 


## Known limitations

- The sniffer code was tested on only one PM5 with only one reader family (Flipper Zero) and only DESFire EV3 cards. Readers that modulate more shallowly at 424 / 848 kbit/s were untested.


- 848 kbit/s needs good coupling; it fails more often than other rates, but is still decently tolerant of placements.


- An asymmetric PPS (different rates each way) involving 848 kbit/s is untested and likely not supported by this firmware. I know of no reader that uses asymmetric rates.


- Trace timestamps wrap after 316.7 s (`trace list` does not unwrap them), and frames longer than 65,535 carrier cycles (card responses over about 56 bytes at 106 kbit/s) show a clamped duration. Both are the same as stock firmware.


- A WUPA that ends a high-rate session is decoded before the switch back to 106 kbit/s and is not edge-timed.


- A sniff started mid-session at a higher rate logs a few fragments until the next WUPA. Higher sniff rates are only activated by a successful PPS negotiation.


- With the card sandwiched tightly between the PM5 and the reader, ending a sniff can disturb the transaction in progress (the stock sniffer does the same).

## Credits

Built on the RfidResearchGroup proxmark3 repository and the open-source PM5 FPGA sources. GPLv3.
