# Private gencord stereo receiver (stdlib only)

**Publishes two mono WAV channels, not transcripts.** Linux/POSIX Python 3.11+;
no dependencies, external binaries, network clients, cloud, STT engine or separate
watcher. This change does **not** install/start a service or modify CT102.

The actual upstream `src/main.cpp` `davRequest` / `uploadOne` contract was read
before implementation: Basic auth; fixed Content-Length; PUT `name.wav.part`;
HEAD byte-size verification; absolute-URL MOVE with `Overwrite: T`; final HEAD.
The original upstream writer is mono and is **deliberately rejected**. Use the
ADV/DJI firmware that genuinely records **two channels**, not duplicated mono.
`TX1` means input channel 1 and `TX2` means input channel 2. These are channel
identities, **not identified people**; DJI transmitter mapping must be verified
on the physical receiver/firmware independently.

## Output and existing Mac workflow

With `--recordings-root /opt/ncdata/data/ncp/files/recordings`, files appear in
its **lowercase** `cardputer/` child:

```
<original-stem>-<publication-id>-TX1.wav
<original-stem>-<publication-id>-TX2.wav
<original-stem>-<publication-id>-stereo.wav.source
<original-stem>-<publication-id>.json
.gencord-staging/                 # PRIVATE, never an audio input
```

The publication id is the full SHA-256 of the input filename, NUL separator and
source-file SHA-256. Outputs are deterministic and never replaced. Two channels
are independently valid mono PCM16/16kHz WAVs with sample-exact deinterleaving.
JSON includes source hash/size, frame count, channel index/identity, mono WAV and
PCM hashes and `transcription: "not performed"`.

The server retains the completed stereo input in private `state/inbox/*.wav`
and a byte-exact archive ending in **`.source`, not `.wav`**. The existing
personal Mac Voice Buddy recursively processes supported WAV files and ignores
`.source`: the interface is two mono audio files for that existing local job.
There is no new transcription service here. The personal Mac job was **not
executed or verified by these tests**; an automation Air is a different machine,
not proof of the personal Voice Buddy workflow. Publication is not transcription
completion. Sync and actual Mac transcription remain separately verified steps.

## Wire restrictions and safety

* Only `/inbox` and one filename below it; ASCII stem of 1–96 characters:
  `[A-Za-z0-9][A-Za-z0-9_-]*`, followed by `.wav` or `.wav.part`.
  Firmware's timestamp/name scheme fits this. PUT only accepts `.wav.part`;
  only MOVE to the **same stem** `.wav` creates a complete/importable file.
* MKCOL `/inbox` returns 405 (already exists), which firmware accepts. HEAD
  returns exact byte Content-Length, no body. PUT returns 201/204; MOVE 201/204.
  No GET, DELETE, directory listing or PROPFIND (HEAD works, no fallback needed).
* Authentication covers supported operations. Credentials exist only in the
  process environment, `GENCORD_USER` and `GENCORD_PASSWORD`. Neither request
  headers, raw paths, audio payloads nor credentials are logged. Diagnostics are
  fixed generic messages. Password rotation requires a process restart.
* Body framing requires **one positive decimal Content-Length**, at most
  536870912 bytes (512 MiB), configurable downward with `--max-bytes`.
  Transfer-Encoding/chunked and Expect/100-continue are rejected. Body streaming
  uses 64 KiB buffers; splitting uses at most 16384 stereo frames per chunk.
* The entire header has a separate **5-second absolute deadline**, including
  the first request line; idle socket timeout defaults to 20 seconds. The body
  has a separate **600-second absolute deadline** that trickle bytes do not
  reset. Flags: `--header-timeout`, `--timeout`, `--body-timeout`. One active HTTP
  handler, one publisher and at most one header-timer thread: no thread-per-peer
  growth. Slow headers are disconnected, not necessarily sent an HTTP response.
* One active PUT temp file at a time. `--max-inbox-bytes` defaults to 4294967296
  (4 GiB), conservatively including retained ready inputs and any crash leftovers
  **plus a proposed upload**; `--max-files` defaults to 1024 and cannot exceed it.
  HTTP 507 refuses new input on budget exhaustion. This bounds queue occupancy
  too. A completed MOVE retains its original and is therefore not a cleanup step.
  Retransmission needs temporary headroom even for identical input.
* `--reserve-bytes` defaults to 268435456 (256 MiB). PUT reserves upload size;
  publisher requires approximately twice the source size plus reserve available
  for its temporary work. Free-space checks are conservative admission checks,
  not filesystem quotas; unrelated writers can still exhaust shared storage.
* Uploads go to O_EXCL temporary files, are validated, fsynced, and linked into
  the private inbox without replacing an existing inode. The directory is fsynced.
  Exact duplicate retries succeed; different bytes return 409, **even with
  `Overwrite: T`**. This intentionally narrows general WebDAV overwrite semantics.
  `Overwrite: F` is also accepted but never grants replacement permission.
* Request targets are validated from the **original request line** (stdlib
  normalizes leading `//` otherwise), URL-decoded **once**, then checked against
  the filename grammar. Traversal, remaining percent escapes/double encoding,
  slashes in a filename, backslashes, queries/fragments and hidden files fail.
* Destination must start with the exact configured `http://host:port/` origin;
  foreign authority, userinfo, protocol changes, alternate DNS names/ports and
  traversal are rejected. **Host header is never trusted.** Origin is explicit
  in service mode, independent of the bind address. IPv4/DNS origins only.
* Directory paths are opened component-by-component with O_NOFOLLOW and pinned
  Linux O_PATH directory descriptors for ancestors/traversal-only recordings
  root (no directory-read permission required there); owned working directories
  are then opened readably for fsync. Leaf opens are O_NOFOLLOW, nonblocking and regular-file
  checked. No symlinks are followed, including config-path ancestors. State and
  recording roots must be disjoint. State + private staging are exclusively
  flock-locked against a second receiver/importer (even with a different state
  pointed at the same output).
* Only canonical **44-byte RIFF/WAVE PCM16, 16000 Hz, 2 channels**, nonempty and
  exact advertised RIFF/data lengths, correct byte rate/alignment and whole
  four-byte frames are accepted. Ancillary chunks, extensible WAV, float PCM,
  mono, wrong rate/width, extra bytes and truncated/torn frames are rejected
  (422). This is deliberately narrower than a general-purpose WAV decoder.

## Durable asynchronous publication and recovery

MOVE validates and **durably commits the stereo source first**, then queues a
single background publisher and responds without waiting for channel splitting.
HEAD stays responsive while splitting. The firmware's 30-second response window
therefore does not include a full split. Hashing/fsync of the intake still takes
storage-dependent time; a lost response is safe to retry. MOVE success means
**durable intake**, not successful split/transcription. Never delete private
inbox WAVs to resolve a publication failure without an independent backup.

The publisher validates the whole source before exposing any mono channel.
A private transaction directory on the **output filesystem** holds non-audio
`.blob` staging files and an fsynced manifest. This permits no-overwrite hard-link
publication even if state and output are on different filesystems. It preflights
**all** existing final files against their expected SHA-256 + size before linking
anything missing. Source archive and provenance are linked before the two mono
files. Existing persisted journal and staged blobs are also verified, not trusted
blindly. Restart regenerates expected bytes deterministically and verifies the
journal before continuing. Corruption or collisions fail closed; no auto-repair
or overwrite of suspicious files.

Two filenames cannot become visible atomically as a group on POSIX. A crash may
leave one complete validated channel visible; after restart, a retry verifies it
and publishes only the missing channel. No half-written `.wav` is ever exposed.
The inbox original is never deleted. File-level data/metadata fsyncs do not
replace filesystem/device durability guarantees.

The same service scans ready inputs once at startup; each MOVE schedules its
filename. Background errors emit a generic pending-publication notice, retain
source, and do **not** spin or erase files. Retry with a new MOVE, restart, or
stop the service and use `--import-once`. There is no separate polling daemon.
Offline `--import-once` fails fast on a corrupt/colliding input. A running service
holds exclusive locks, so a simultaneous offline importer/check is refused.

Private staging persists for recovery. Published outputs are hard links to its
verified blobs: treat outputs as immutable; editing them in place also changes
staged content and subsequent verification will refuse them. Copy to a distinct
filename if manual edits are needed. Approximate physical retention is three
source sizes (inbox source + staged/archive source + both mono channels), with
up to two more source sizes while verifying/rebuilding a publication. Abrupt
termination can retain private `.work-*`, `.tmp-*`, or inbox `.upload-*` files;
they are not audio inputs, count against storage headroom, and are **not**
automatically deleted. Offline operator-reviewed retention/cleanup is necessary
for a long-running archive; do not remove a committed manifest/blobs casually.

## Run/check/import

Run from this directory, with credentials set privately in your environment:

```sh
python3 gencord_receiver.py \
  --bind 192.168.1.102 --port 8795 --origin http://192.168.1.102:8795 \
  --state /var/lib/gencord-receiver \
  --recordings-root /opt/ncdata/data/ncp/files/recordings
```

Firmware WebDAV URL: `http://192.168.1.102:8795/inbox`, matching the endpoint
origin exactly. Set its username/password separately; don't commit/export a
credential-bearing firmware configuration. Basic auth over HTTP is **not
encrypted**. This is a trusted private-LAN endpoint, not safe for Internet,
untrusted Wi-Fi, a public reverse proxy or tunnel. Restrict firewall access to
actual recorder/LAN clients. No TLS implementation is bundled.

Offline operations (service stopped; credentials not needed):

```sh
python3 gencord_receiver.py --state /var/lib/gencord-receiver \
  --recordings-root /opt/ncdata/data/ncp/files/recordings --check
python3 gencord_receiver.py --state /var/lib/gencord-receiver \
  --recordings-root /opt/ncdata/data/ncp/files/recordings --import-once
```

`--check` checks bounds, directory confinement and exclusive locks; it **creates
owned directories** and enforces private-state/staging permissions. It does not
start a listener, validate WAV contents, check credentials, test sync/STT, or
prove available long-term disk capacity. Run it under the intended service UID.
State must already be an appropriately chosen dedicated path, never an existing
unrelated directory. Root users must not point configuration at arbitrary data.

## Deployment example for parent review — not applied

`gencord-receiver.service` is a **single-service example**, not installed here.
It runs as dedicated **nonlogin `gencord-receiver`**, with its own primary group,
not as `www-data` and not as a supplementary member of that broad group. Root
installs the script read-only at `/opt/gencord-receiver/gencord_receiver.py` and
unit at `/etc/systemd/system/gencord-receiver.service`. Parent must verify CT102
paths, UID/GID, filesystem/link support, ownership and policy before enabling.

Example provisioning commands to be reviewed/executed by the parent **inside
CT102**, not on the PVE host and not by these tests:

```sh
useradd --system --user-group --home-dir /nonexistent --no-create-home \
  --shell /usr/sbin/nologin gencord-receiver
install -d -o gencord-receiver -g gencord-receiver -m 0700 /var/lib/gencord-receiver
install -d -o gencord-receiver -g www-data -m 2770 \
  /opt/ncdata/data/ncp/files/recordings/cardputer
```

Only apply ownership/mode to the dedicated **cardputer child**, never recursively
to the recordings root or unrelated files. Ancestors must grant the dedicated
UID directory traversal (not broad read/write); add a narrow execute-only ACL
if needed after inspection. Output setgid preserves group `www-data` into
private staging and all final 0640 output inodes without adding the service user
to `www-data`. New private staging subdirectories inherit setgid but remain
owner-only accessible (2700). The receiver preserves this bit rather than
silently clearing it. Syncthing (running as `www-data`) can read final outputs.

**Ignore `cardputer/.gencord-staging` in this sync folder's Syncthing ignore
configuration** before use; the directory is intentionally unreadable by the
sync account and must not be synced. Root may need to perform that configuration
in the existing sync folder; this code does not edit `.stignore` or sync settings.
Verify the existing audio scanner selects TX1/TX2 and not `.source`/`.blob`;
no claim is made that this receiver test exercised a personal Mac scanner.

Create `/etc/gencord-receiver.env` locally, root:root mode 0600, containing the
`GENCORD_USER` and `GENCORD_PASSWORD` values. No secrets or env template values
are stored here. The unit uses `ProtectSystem=strict` and **ReadWritePaths only
for state and the cardputer child**; existing recordings are not write targets.
It applies umask 0027, empty capabilities, a 128 MiB memory cap, task cap 8,
file-descriptor cap 64, and a 30-second stop grace period. An interrupted worker
is recoverable from durable source/manifest. Normal directory ownership/ACL
boundaries remain necessary; systemd write confinement is not a substitute for
least-privilege read access. Actual sandbox/UID/sync behavior must be verified
on CT102 by the deploying parent.

## Local proof

```sh
python3 -m unittest -v test_receiver
```

All network tests bind loopback ephemeral ports and use TemporaryDirectory;
no live recordings, credentials, CTs or services are used. Tests cover actual
PCM WAV files, real HTTP auth/PUT/HEAD/MOVE, exact PCM hashes/isolation, original
retention, malformed/mono/torn inputs, framing/size rejection, slow-header and
slow-body absolute deadlines, foreign/malformed Destination, request-target
attacks, symlinks, collisions, simultaneous PUTs, duplicate retry, partial
publication + restart, journal/blob corruption, budget refusal, async response
before split, process locking and setgid output. Fault injection is only at
filesystem link/free-space boundaries; HTTP and WAV I/O are real. Simulated
interruption tests do not establish physical power-loss durability.

Retained `RED-*.log` / `GREEN-*.log` and `ACTIONS-receiver.md` record actual
TDD executions, including failures discovered and fixed. Parent independently
reviews and decides deployment; no commits were made by this receiver task.
