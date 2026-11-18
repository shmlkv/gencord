# gencord ADV — independent DJI microphone channels

A focused adaptation of [genkigenki/gencord](https://github.com/genkigenki/gencord) for **M5Stack Cardputer ADV** and a USB DJI Mic Mini receiver with two transmitters.

**Test firmware, not yet physically qualified.** Compilation and host tests do not prove USB compatibility with a particular receiver/cable/power arrangement.

## Contract

- USB-only recording: require a supported **two-channel** USB audio stream.
- Preserve left/right independently in a **16 kHz, PCM16 stereo WAV** on microSD.
- No mixing to mono, no silent fallback to the internal microphone.
- Stop/finalize visibly on capture failure; retain original SD recordings after upload.
- The receiver must be set to **S (Stereo)**. Two reported USB channels alone cannot distinguish S from a mono mix duplicated across L/R.
- Isolate this recorder's configuration and recordings from other firmware's SD data.

## Existing archive integration

[`receiver/`](receiver/) is a narrow authenticated local-network WebDAV receiver, compatible with the firmware's PUT → HEAD → MOVE → HEAD protocol. It validates stereo PCM and publishes:

- `*-TX1.wav`: first channel, unchanged sample values;
- `*-TX2.wav`: second channel, unchanged sample values;
- `*-stereo.wav.source`: byte-identical stereo original with a non-audio suffix;
- provenance JSON with channel mapping and hashes.

An existing recursive local transcription pipeline can process the two mono WAVs separately. The non-audio `.source` suffix avoids accidentally transcribing a mixed version a third time. The receiver itself **does not perform ASR or claim transcripts are ready**.

Basic authentication over plain HTTP is suitable only for an explicitly trusted LAN; it does not encrypt credentials/audio. Do not expose it publicly. The firmware must not be configured with a shared personal Nextcloud password: use scoped receiver credentials, stored outside source control.

## Power

Use a proper USB OTG host/data adapter with a designed external-power input. Upstream documents that the Cardputer USB-C does not provide VBUS to the receiver. A random USB-C charging splitter is not equivalent. Neither the concrete adapter nor actual DJI USB descriptors have been physically verified here.

## Build, tests and operation

- [Russian owner quickstart](ADV-QUICKSTART.ru.md)
- [Firmware build report](BUILD-REPORT.md)
- [Firmware action log](ACTIONS-firmware.md)
- [Receiver setup and tests](receiver/README.md)
- [Verification boundaries](QUALIFICATION.md)

The upstream MIT license remains in [LICENSE](LICENSE). The vendored Espressif USB Host UAC component has its own Apache-2.0 license. Original upstream history and features remain available through Git; old generic flashing instructions are not safe for a customized owner partition table.

## Installation safety

**Do not flash a merged/full image, erase flash or use an address copied from upstream instructions.** This application does not run as a Nucleo `.nap` plugin. It needs its own sufficiently large executable app partition, or an explicitly approved replacement of an existing application.

Discover the actual device and current partition map, verify an exact rollback backup, select the authorized target, and install **app-only** with readback verification. A saved historical map is not a fresh on-device read. No automatic installer in this repository is authorization to replace another firmware.

## Physical acceptance still required

ADV keyboard/display/SD; powered USB receiver enumeration; one-transmitter-at-a-time separation; disconnect finalization; actual Wi-Fi upload with SD retention; archive synchronization; and two real channel transcripts from the existing ASR runtime. Until those pass, do not use this as the only recorder for important conversations.
