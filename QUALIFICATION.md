# Local verification notes

## Existing transcript scanner qualification

Read-only inspection of the existing Voice Buddy scanner found recursive discovery of `.m4a`, `.mp3`, `.wav`, `.ogg`, `.webm`, excluding hidden entries, `_tools`, `node_modules` and symlinks.

The actual scanner module was copied outside this repository into a disposable qualification workspace (no private recording payloads). A real Node 22 invocation on a temporary directory containing synthetic names selected exactly:

- `cardputer/test-TX1.wav`
- `cardputer/test-TX2.wav`

It excluded `test-stereo.wav.source`, JSON metadata and a hidden incomplete Syncthing file. This verifies ingestion-name compatibility only; no real microphone capture, speech recognition or Mac scheduler execution was demonstrated by this test.

## One-hour bounded-memory publication benchmark

A disposable host-side run used 57,600,000 stereo frames (230,400,044-byte canonical WAV), with synthetic constant +1000/-1000 channels, not speech and not device capture. It exercised actual Receiver.put/move/publication using a file body and a real socket only as the timeout-control object (not HTTP/network throughput).

Observed: PUT 0.346 s, synchronous MOVE+publication 1.276 s, process max RSS 21,820 KiB. Both mono WAVs had the exact expected frame count and sampled values; the complete retained stereo source SHA-256 matched the input. The disposable tree was removed by TemporaryDirectory cleanup. These timings apply only to this host/storage, not the CT, ESP32, Wi-Fi or USB link.

Reviewed benchmark source dictionary entry: receiver SHA-256 `87d88c36a29b316e7f42daf46840954771f69490717af48de815d867d30f24f4`; subsequent receiver changes require fresh relevant checks, not inherited approval.

## Device availability and installation boundary

Read-only request through the existing Mac route did not return a reachable Cardputer. No button events, flashing, repartitioning or SD writes were performed.

The saved owner-specific partition map has Nucleo and Switch Bridge already allocated. The upstream example addresses are not installation authority. An app-only BIN must not be installed until a fresh device inventory, selected target partition, backup and explicit replacement scope exist.

## Distinct transcription runtime

The accessible automation Mac did not contain the expected personal Voice Buddy directory and its crontab did not show the personal recording job. It is a different machine, not evidence that the personal Mac's job is missing or broken. Existing synced documentation describes local MLX Whisper on the personal Mac. The complete transcription acceptance remains two actual new `.txt` outputs on that machine after synchronization.
