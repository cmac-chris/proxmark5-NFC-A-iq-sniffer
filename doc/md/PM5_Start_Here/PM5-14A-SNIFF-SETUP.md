# Proxmark5 14a sniffer: install and use

How to put the 14a sniffer on a Proxmark5, sniff with it, test it with a Flipper Zero, and go back to RRG's firmware.
Everything else (building, the bootrom, Boot Mode, recovery) is in RRG's
[Getting Started with the Proxmark5](./Getting_Started_With_PM5.md). How the sniffer works is in
[PM5-14A-SNIFF.md](./PM5-14A-SNIFF.md).

## Contents

- [Before you start](#before-you-start)
- [Install on Windows: one click](#install-on-windows-one-click)
- [Build and flash from source](#build-and-flash-from-source)
- [Sniff](#sniff)
- [Where to put the card](#where-to-put-the-card)
- [Test with a Flipper Zero](#test-with-a-flipper-zero)
- [Go back to RRG's firmware](#go-back-to-rrgs-firmware)
- [Troubleshooting](#troubleshooting)

## Before you start

- **Your PM5 needs RRG's bootrom.** If you have flashed it from RRG's repository before, it has it. If not, do RRG's
  [Flash the Bootrom](./Getting_Started_With_PM5.md#flash-the-bootrom) step once, then come back.
- **Only the ARM firmware (the fullimage) is flashed.** The bootrom and the FPGA's flash are never written. The
  sniffer's FPGA image is part of the firmware and goes into the FPGA's RAM only while `hf 14a sniff` runs. Every other
  command works as in RRG's firmware.
- **The firmware and the client go together.** This client turns the new sniffer on and reads traces over 64 KB. A stock
  client on this firmware gets the old 106 kbit/s sniffer, and so does this client on stock firmware.

## Install on Windows: one click

1. Download `pm5-14a-sniff-v1.0-win64.zip` from the [Releases](https://github.com/cmac-chris/proxmark5-NFC-A-iq-sniffer/releases) page.
   Right-click it, **Properties**, tick **Unblock**, **OK**, then unzip it anywhere.
2. Plug the PM5 into the USB port on the button side. Close any other program that uses it: only one can.
3. Double-click `flash.bat`. It finds the PM5 by its USB ID, asks before flashing, flashes the firmware (about 30 s),
   waits for the PM5 to come back and opens the client.

From then on, `pm3.bat` opens the client.

| In the zip | |
|---|---|
| `flash.bat` | flashes `firmware\fullimage.elf`, then opens the client |
| `pm3.bat` | opens the client on the PM5 |
| `flash_stock.bat` | puts RRG's firmware back: see [Go back](#go-back-to-rrgs-firmware) |
| `firmware\` | this firmware, and RRG's built from the same commit |
| `client\` | the client with its DLLs, dictionaries and scripts |
| `SHA256SUMS.txt` | a checksum for every file |

The Releases page gives the zip's SHA-256. To check it, in PowerShell: `Get-FileHash .\pm5-14a-sniff-v1.0-win64.zip`.

## Build and flash from source

On Linux, macOS, WSL or ProxSpace, set up as in RRG's guide, then:

```
git clone https://github.com/cmac-chris/proxmark5-NFC-A-iq-sniffer.git
cd proxmark5-NFC-A-iq-sniffer
make -j PLATFORM=PM5
./pm3-flash-fullimage
./pm3
```

- **No FPGA tools are needed.** The sniffer's FPGA image is in the repository, prebuilt
  (`fpga/fpga_pm5_sniff14a.bin`), and the build puts it into the firmware. To rebuild it:
  `make -C fpga fpga_pm5_sniff14a` (yosys, nextpnr-himbaechel and apicula, as in
  [oss-cad-suite](https://github.com/YosysHQ/oss-cad-suite-build)).
- **The firmware is about 570 KB**, so the PM5's `PLATFORM_SIZE` now defaults to 1024 KB. If your `Makefile.platform`
  sets a smaller size, remove that line.
- **`make -j PLATFORM=PM5 PM5_SNIFF_IMAGE=`** builds without the sniffer image: `hf 14a sniff` is then RRG's
  sniffer at 106 kbit/s, as with `--stock`.

## Sniff

```
hf 14a sniff
```

Let the reader and the card talk, then stop the sniff with the PM5's button or any client command, such as `hw break`.
It also stops when the buffer is full: about 420 KB, on the order of 10,000 frames. Then list what it caught:

```
hf 14a list
```

- **Loading:** the sniffer image loads in about 50 ms. RRG's image comes back about 200 ms after the sniff ends.
- **Options** are as before: `-c` starts logging at the card's first answer, `-r` at the first REQA or WUPA, and `-i`
  keeps the console until the sniff ends (Enter stops it).
- **Rates:** after a PPS the card confirms, each direction is decoded at its new rate. A REQA or WUPA returns both to
  106 kbit/s.
- **Start the sniff before the reader finds the card.** A sniff that starts in the middle of a session at a higher rate
  logs a few fragments until the next WUPA.
- **Times** are in carrier cycles (1 cycle = 73.75 ns); `hf 14a list -u` shows microseconds. Timestamps wrap after
  316.7 s, and `hf 14a list` does not unwrap them.
- **Saving:** `trace save -f <name>` saves the trace. Open it later with `trace load -f <name>`, then
  `trace list -t 14a -1`.

### Testing options

These are for checking the sniffer, and comparing it with the old one:

| Command | What it does |
|---|---|
| `hf 14a sniff --stock` | RRG's sniffer exactly as it is, with no fixes: the image in the FPGA's flash, 106 kbit/s only. Run the same reads with and without it for a direct comparison. |
| `hf 14a sniff -d` | after the sniff (it stops with the button or Enter), checks every frame and prints **PASS** or **FAIL**, the error count, the number of frames and the PPS rates seen. Errors: a bad CRC, BCC or parity, a reader frame with no answer, a card frame too short to be anything. |
| `hw dbg -3` | from then on, every sniff also prints how long the sniffer image took to load and RRG's image to come back. `hw dbg -1` turns it off again. |

`-d` judges each frame by the protocol alone, so it needs no reference. A trace that starts or ends in the middle of
an exchange can count one error at either end.

## Where to put the card

The sniffer was tested in eight placements, from the card on the bare antenna to 4.4 cm above the cover (see
[Results](./PM5-14A-SNIFF.md#81-placements-and-coupling)). In short:

- **106, 212 and 424 kbit/s** worked in every placement.
- **848 kbit/s** needs the card over the antenna: about 2 cm off centre it fails.
- **In a tight stack** (card on the PM5, reader on top), an idle PM5 can keep the reader from reading the card. Start
  the sniff first. A sniff that stops in the middle of a read can make that read fail.

## Test with a Flipper Zero

Most readers never leave 106 kbit/s. The *14A HBR Tester* app makes a Flipper Zero read cards at 106, 212, 424 or
848 kbit/s, with no change to the Flipper's firmware. It is in
[flipper_hbr_tester](../../../flipper_hbr_tester), at the top of this repository: the prebuilt `.fap` files, the source and its
own [README](../../../flipper_hbr_tester/README.md).

1. From the [flipper_hbr_tester](../../../flipper_hbr_tester) folder (or the
   [Releases](https://github.com/cmac-chris/proxmark5-NFC-A-iq-sniffer/releases) page), take the `.fap` for your Flipper's firmware:

   | Firmware | File |
   |---|---|
   | Official 1.5.1, Unleashed unlshd-094 | `iso14443a_hbr_tester_ofw-1.5.1_api88.2.fap` |
   | Momentum mntm-012, Official 1.4.3, Unleashed unlshd-086 | `iso14443a_hbr_tester_mntm-012_api87.1.fap` |
   | Unleashed unlshd-082 | `iso14443a_hbr_tester_unlshd-082_api86.0.fap` |

   Other firmware with the same API major version takes the same file. If the Flipper says the app or its firmware is
   outdated, try another file.
2. Copy it to the Flipper's SD card, into `apps/NFC/`, with qFlipper or a card reader.
3. On the PM5, start `hf 14a sniff`.
4. On the Flipper, open **Apps → NFC → 14A HBR Tester**, pick a rate (or **Cycle**), place the card and press
   **Read**, or **Loop** for many reads.
5. Stop the sniff and run `hf 14a list`. The Flipper saves its own trace of every read to
   `/ext/apps_data/iso14443a_hbr_tester/`: compare the two frame by frame.

The card must advertise the rate in its ATS. MIFARE DESFire EV1, EV2 and EV3 cards advertise up to 848 kbit/s both
ways. MIFARE Classic, Ultralight and NTAG cards stay at 106.

## Go back to RRG's firmware

Only the fullimage was changed, so flashing RRG's fullimage undoes everything:

- **Windows zip:** double-click `flash_stock.bat`.
- **From source:** in a checkout of RRG's repository, `make -j PLATFORM=PM5`, then `./pm3-flash-fullimage`.

Use RRG's client with RRG's firmware.

## Troubleshooting

- **"PM5 not found":** use the USB port on the button side and a data cable, and close every other program that has
  the PM5 open. In Device Manager it shows under **Ports (COM & LPT)**.
- **The flash stopped part way:** run `flash.bat` again. The bootrom is never touched, so the PM5 can always be
  flashed again. If it no longer starts, see RRG's [Boot Mode](./Getting_Started_With_PM5.md#flash-the-bootrom) and
  [DFU](./Getting_Started_With_PM5.md#dfu-install) steps.
- **The client warns that the firmware does not match:** the client and the firmware come from different builds. Use
  the client from the same zip or build.
- **After a PPS, `hf 14a list` shows only broken frames:** the old sniffer ran. Check that both the firmware and the
  client come from this repository (`hw version`).
- **Nothing decoded at 848 kbit/s:** move the card over the centre of the antenna.
