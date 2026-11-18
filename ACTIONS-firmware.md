# Firmware action log

1. Cloned upstream gencord and preserved its Git history/license. Used an isolated branch and secondary-disk build workspace because host root capacity was tight.
2. Read original capture, WAV, WebDAV and SD configuration paths. Replaced mono mixing with a testable fixed-memory independent-channel PCM pipeline.
3. Switched to USB-only failure behavior, ADV runtime board/keyboard support, isolated `/gencord-adv` SD paths, retained originals and upload markers. Removed legacy keyboard/audio sleep pin driving from ADV behavior.
4. Added host tests before production PCM implementation; retained initial RED and subsequent GREEN evidence under ignored `logs/`. Added actual-producer and extracted-UAC-callback host harnesses, including driver overflow/errors and stall handling.
5. Addressed concrete build issues: M5 keyboard API field differences, esptool/click compatibility, IDF54 RX FIFO bias limitation. Dependencies and compiler downloads remain inside the secondary-disk build workspace.
6. Parent re-executed all four production-source host suites and the real PlatformIO build, validated the actual app image with esptool. See BUILD-REPORT.md for exact final artifact/hash, warning and limits.
7. No physical hardware, USB serial, bootloader, partition table, Nucleo or Switch Bridge was written. Device acceptance is still blocked by connection and an authorized app-partition selection.

This log summarizes actions proven by retained tool output/source; it does not convert synthetic harness results into physical compatibility claims.
