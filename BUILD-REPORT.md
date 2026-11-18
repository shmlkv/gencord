# Verified build report

## Actual executed build

`bash tools/build_firmware.sh` completed with **SUCCESS** using Arduino 3.2.1 / ESP-IDF 5.4.2 prebuilt libraries, PlatformIO environment `adv-idf54`, two compile jobs. App-only artifact: `gencord-adv-app.bin`, **1,479,680 bytes**, SHA-256 `fd8303b3f8d3239a63ca751abbac69119eb14d8bb950c8eb8035a195279e747f`.

Static RAM: 148,452 / 327,680 bytes. Linker flash usage: 1,479,138 / 3,342,336 bytes. These are static/linker budgets, not measured runtime free heap. Image metadata verified ESP32-S3, 8MB flash, DIO/80MHz, valid checksum and appended validation hash. Prebuilt Arduino project metadata's older compile date is not the application build timestamp.

Nonfatal tooling warning: esp-idf-size rejected `--ng`; PlatformIO continued and its size check plus app image generation succeeded. This warning was not hidden or described as a wholly warning-free build.

## Actual host tests

`python3 tests/run_host.py`: four suites pass under ASan/UBSan:

1. Actual stereo PCM conversion/resampling, fragmented packets, L/R isolation, anti-phase preservation, frame counts, WAV header and termination errors.
2. Actual USB producer implementation with synthetic host APIs and real local threads/queues: mono rejection, short final blocks, normal/disconnect/transfer/overflow/stall termination and restart.
3. IDF54 RX FIFO wrapper calls the real HAL boundary with RX bias (synthetic HAL test, not physical register observation).
4. Extracted actual vendored UAC RX callback: valid data, ring overflow, bad packets, push/submit failures and malformed frame lengths.

These are actual production-source executions against synthetic hardware boundaries, NOT real DJI/USB/ADV acceptance.

Receiver: `cd receiver && python3 -m unittest -q test_receiver`: **32 tests pass**. The same receiver source and tests also passed on Python 3.11.2 in a disposable CT102 directory. Invoking discovery from the repository root failed one subprocess fixture import due to working-directory assumptions; the supported invocation above passed. No production receiver service had been installed when those tests were run.

## Intentional behavior and limits

- Input must be exactly two channels in a supported UAC1 PCM format; output fixed PCM16 stereo16kHz, no automatic internal-mic fallback or cross-channel gain/mixing.
- Capture stops after **60 minutes**; this version does NOT promise seamless automatic segment rollover.
- SD source/config isolated under `/gencord-adv`. Originals remain after upload; persisted upload markers suppress redundant transfers.
- Driver failures stop capture rather than silently omitting packets. A startup/stream stall ends with an error.
- The IDF54 prebuilt USB FIFO default is unsuitable for some stereo input packet sizes; this app wraps the HAL FIFO-bias boundary and retains the real HAL's configuration/limits. Physical compatibility still requires testing.

## Installation blocked by physical availability and partition scope

No device was flashed, no partition table was changed, no existing Nucleo/Launcher/Switch Bridge/SD data was overwritten. This BIN is larger than the saved map's free flash tail; existing app partitions are occupied. It must not be installed by guessing a free address, nor by replacing Nucleo without separate approval and a fresh verified backup.

Actual two-transmitter USB capture, power adapter compatibility, on-device UI/SD, Wi-Fi upload from the device and personal-Mac transcript outputs remain unverified.
