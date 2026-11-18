#!/usr/bin/env python3
"""Confined gencord DAV intake and deterministic two-channel PCM publication.

Linux/POSIX, Python >=3.11; standard library only. No STT or cloud calls.
"""
import argparse
import base64
import contextlib
import fcntl
import hashlib
import hmac
from http.server import BaseHTTPRequestHandler, HTTPServer
import json
import os
from pathlib import Path
import re
import shutil
import socket
import stat
import struct
import threading
import time
from urllib.parse import unquote, urlsplit
import uuid
import wave

CHUNK = 64 * 1024
MAX_BYTES = 512 * 1024 * 1024
NAME = re.compile(r'[A-Za-z0-9][A-Za-z0-9_-]{0,95}\.wav(?:\.part)?\Z')
NOFOLLOW = os.O_NOFOLLOW | os.O_CLOEXEC


class ReceiverError(Exception):
    def __init__(self, status=409):
        self.status = status
        super().__init__('receiver operation refused')


def directory(path, path_only=False):
    """Pin every directory component; never traverse a symbolic link."""
    path = Path(os.path.abspath(path))
    fd = os.open('/', os.O_PATH | os.O_DIRECTORY | NOFOLLOW)
    try:
        for component in path.parts[1:]:
            try:
                os.mkdir(component, 0o750, dir_fd=fd)
            except FileExistsError:
                pass
            new = os.open(component, os.O_PATH | os.O_DIRECTORY | NOFOLLOW, dir_fd=fd)
            os.close(fd)
            fd = new
        if path_only:
            return fd
        readable = os.open('.', os.O_RDONLY | os.O_DIRECTORY | NOFOLLOW, dir_fd=fd)
        os.close(fd)
        return readable
    except BaseException:
        os.close(fd)
        raise


def child_directory(parent, name, mode=0o700):
    try:
        os.mkdir(name, mode, dir_fd=parent)
        # parent can be an O_PATH traversal-only handle to recordings root.
        sync_fd = os.open('.', os.O_RDONLY | os.O_DIRECTORY | NOFOLLOW, dir_fd=parent)
        try:
            os.fsync(sync_fd)
        finally:
            os.close(sync_fd)
    except FileExistsError:
        pass
    fd = os.open(name, os.O_RDONLY | os.O_DIRECTORY | NOFOLLOW, dir_fd=parent)
    if mode == 0o700:
        current = os.fstat(fd).st_mode
        # Keep inherited setgid so a dedicated service UID need not join www-data.
        # Avoid chmod when already private: an unprivileged non-group owner may
        # lose inherited SGID if it explicitly re-applies it through chmod.
        if stat.S_IMODE(current) & 0o777 != mode:
            os.fchmod(fd, mode | (current & stat.S_ISGID))
            if current & stat.S_ISGID and not os.fstat(fd).st_mode & stat.S_ISGID:
                os.close(fd)
                raise ReceiverError(409)
    return fd


def regular(parent, name):
    try:
        fd = os.open(name, os.O_RDONLY | NOFOLLOW | os.O_NONBLOCK, dir_fd=parent)
    except FileNotFoundError:
        raise
    except OSError:
        raise ReceiverError(409) from None
    if not stat.S_ISREG(os.fstat(fd).st_mode):
        os.close(fd)
        raise ReceiverError(409)
    return os.fdopen(fd, 'rb')


def digest_file(stream):
    stream.seek(0)
    digest = hashlib.sha256()
    size = 0
    while block := stream.read(CHUNK):
        digest.update(block)
        size += len(block)
    stream.seek(0)
    return {'sha256': digest.hexdigest(), 'bytes': size}


def validate_wav(stream, maximum):
    """Only the firmware's canonical 44-byte RIFF header is accepted."""
    size = os.fstat(stream.fileno()).st_size
    stream.seek(0)
    header = stream.read(44)
    if len(header) != 44 or not 44 < size <= maximum:
        raise ReceiverError(422)
    fields = struct.unpack('<4sI4s4sIHHIIHH4sI', header)
    expected = (b'RIFF', size - 8, b'WAVE', b'fmt ', 16, 1, 2,
                16000, 64000, 4, 16, b'data', size - 44)
    if fields != expected or (size - 44) % 4:
        raise ReceiverError(422)
    stream.seek(0)
    with wave.open(stream, 'rb') as wav:
        if wav.getnframes() != (size - 44) // 4:
            raise ReceiverError(422)
    stream.seek(0)
    return (size - 44) // 4


def atomic_private(parent, name, writer):
    """Replace only private staging metadata/blobs, never a public recording."""
    temp = '.tmp-' + uuid.uuid4().hex
    fd = os.open(temp, os.O_WRONLY | os.O_CREAT | os.O_EXCL | NOFOLLOW, 0o640, dir_fd=parent)
    try:
        with os.fdopen(fd, 'wb') as stream:
            writer(stream)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temp, name, src_dir_fd=parent, dst_dir_fd=parent)
        os.fsync(parent)
    finally:
        with contextlib.suppress(FileNotFoundError):
            os.unlink(temp, dir_fd=parent)


def canonical_json(value):
    return (json.dumps(value, sort_keys=True, separators=(',', ':')) + '\n').encode('ascii')


def file_matches(parent, name, expected):
    try:
        with regular(parent, name) as stream:
            if os.fstat(stream.fileno()).st_size != expected['bytes'] or digest_file(stream) != expected:
                raise ReceiverError(409)
        return True
    except FileNotFoundError:
        return False


class Receiver:
    def __init__(self, state, recordings_root, max_bytes=MAX_BYTES, timeout=20,
                 header_timeout=5, body_timeout=600, max_inbox_bytes=4 * 1024**3,
                 max_files=1024, reserve_bytes=256 * 1024**2):
        if not 44 < max_bytes <= MAX_BYTES or not 0 < timeout <= 300:
            raise ValueError('invalid bounds')
        self.max_bytes, self.timeout = max_bytes, timeout
        if not 0 < header_timeout <= 60 or not 0 < body_timeout <= 3600:
            raise ValueError('invalid deadlines')
        if not 1 <= max_files <= 1024 or max_inbox_bytes <= 44 or reserve_bytes < 0:
            raise ValueError('invalid storage budget')
        state_path, output_path = os.path.abspath(state), os.path.abspath(recordings_root)
        if os.path.commonpath((state_path, output_path)) in (state_path, output_path):
            raise ValueError('state and recordings roots must be disjoint')
        self.header_timeout, self.body_timeout = header_timeout, body_timeout
        self.max_inbox_bytes, self.max_files, self.reserve_bytes = max_inbox_bytes, max_files, reserve_bytes
        self.lock = threading.RLock()
        self.publish_lock = threading.RLock()
        self.condition = threading.Condition()
        self.pending, self.scheduled = [], set()
        self.worker = None
        self.stopping = False
        self.fds = []
        try:
            self.state = self._own(directory(state))
            # State is explicitly private; never chmod existing recordings directories.
            os.fchmod(self.state, 0o700)
            self.lease = self._own(os.open('.lock', os.O_RDWR | os.O_CREAT | NOFOLLOW,
                                           0o600, dir_fd=self.state))
            if not stat.S_ISREG(os.fstat(self.lease).st_mode):
                raise ReceiverError()
            fcntl.flock(self.lease, fcntl.LOCK_EX | fcntl.LOCK_NB)
            self.inbox = self._own(child_directory(self.state, 'inbox'))
            self.recordings = self._own(directory(recordings_root, path_only=True))
            self.output = self._own(child_directory(self.recordings, 'cardputer', 0o750))
            self.staging = self._own(child_directory(self.output, '.gencord-staging'))
            # Protect publication even if two instances accidentally use different state roots.
            self.output_lease = self._own(os.open('.lock', os.O_RDWR | os.O_CREAT | NOFOLLOW,
                                                  0o600, dir_fd=self.staging))
            if not stat.S_ISREG(os.fstat(self.output_lease).st_mode):
                raise ReceiverError()
            fcntl.flock(self.output_lease, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BaseException:
            self.close()
            raise

    def _own(self, fd):
        self.fds.append(fd)
        return fd

    def close(self):
        self.stop_worker()
        for fd in reversed(self.fds):
            os.close(fd)
        self.fds.clear()

    def start_worker(self):
        self.stopping = False
        self.worker = threading.Thread(target=self._work, name='gencord-publisher', daemon=True)
        self.worker.start()
        with os.scandir(self.inbox) as entries:
            for entry in entries:
                if NAME.fullmatch(entry.name) and entry.name.endswith('.wav'):
                    self.schedule(entry.name)

    def stop_worker(self):
        if self.worker is not None:
            with self.condition:
                self.stopping = True
                self.condition.notify_all()
            self.worker.join()
            self.worker = None

    def schedule(self, name):
        with self.condition:
            if name in self.scheduled:
                return
            if len(self.scheduled) >= self.max_files:
                raise ReceiverError(503)
            self.scheduled.add(name)
            self.pending.append(name)
            self.condition.notify_all()

    def wait_idle(self, timeout):
        with self.condition:
            return self.condition.wait_for(lambda: not self.scheduled, timeout)

    def _work(self):
        while True:
            with self.condition:
                self.condition.wait_for(lambda: self.pending or self.stopping)
                if self.stopping:
                    return
                name = self.pending.pop(0)
            try:
                self.publish(name)
            except (OSError, ReceiverError, ValueError, wave.Error):
                print('publication pending; retry MOVE, restart or import offline', flush=True)
            finally:
                with self.condition:
                    self.scheduled.discard(name)
                    self.condition.notify_all()

    def require_space(self, directory_fd, needed):
        usage = os.fstatvfs(directory_fd)
        if usage.f_bavail * usage.f_frsize < needed + self.reserve_bytes:
            raise ReceiverError(507)

    def check_intake_budget(self, name, length):
        total = count = 0
        target_exists = False
        with os.scandir(self.inbox) as entries:
            for entry in entries:
                count += 1
                info = entry.stat(follow_symlinks=False)
                total += info.st_size
                target_exists |= entry.name == name
                if count > self.max_files or total + length > self.max_inbox_bytes:
                    raise ReceiverError(507)
        if count >= self.max_files and not target_exists:
            raise ReceiverError(507)
        if total + length > self.max_inbox_bytes:
            raise ReceiverError(507)
        self.require_space(self.inbox, length)

    def put(self, name, body, length, connection):
        self.check_intake_budget(name, length)
        deadline = time.monotonic() + self.body_timeout
        temp = '.upload-' + uuid.uuid4().hex
        fd = os.open(temp, os.O_RDWR | os.O_CREAT | os.O_EXCL | NOFOLLOW, 0o600, dir_fd=self.inbox)
        try:
            with os.fdopen(fd, 'w+b') as stream:
                remaining = length
                while remaining:
                    budget = deadline - time.monotonic()
                    if budget <= 0:
                        raise ReceiverError(408)
                    connection.settimeout(min(self.timeout, budget))
                    block = body.read1(min(CHUNK, remaining))
                    if time.monotonic() >= deadline:
                        raise ReceiverError(408)
                    if not block:
                        raise ReceiverError(400)
                    stream.write(block)
                    remaining -= len(block)
                stream.flush()
                validate_wav(stream, self.max_bytes)
                os.fsync(stream.fileno())
                expected = digest_file(stream)
            if file_matches(self.inbox, name, expected):
                return 204
            os.link(temp, name, src_dir_fd=self.inbox, dst_dir_fd=self.inbox, follow_symlinks=False)
            os.fsync(self.inbox)
            return 201
        finally:
            with contextlib.suppress(FileNotFoundError):
                os.unlink(temp, dir_fd=self.inbox)
            os.fsync(self.inbox)

    def move(self, source, destination, defer=False):
        existed = False
        try:
            with regular(self.inbox, source) as stream:
                validate_wav(stream, self.max_bytes)
                expected = digest_file(stream)
            existed = file_matches(self.inbox, destination, expected)
            if not existed:
                os.link(source, destination, src_dir_fd=self.inbox, dst_dir_fd=self.inbox,
                        follow_symlinks=False)
                os.fsync(self.inbox)
            os.unlink(source, dir_fd=self.inbox)
            os.fsync(self.inbox)
        except FileNotFoundError:
            with regular(self.inbox, destination) as stream:
                validate_wav(stream, self.max_bytes)
            existed = True
        try:
            if defer:
                self.schedule(destination)
            else:
                self.publish(destination)
        except (OSError, ReceiverError, ValueError, wave.Error):
            raise ReceiverError(503) from None
        return 204 if existed else 201

    def import_once(self):
        with self.publish_lock:
            count = 0
            # scandir streams names instead of allocating the whole inbox.
            with os.scandir(self.inbox) as entries:
                for entry in entries:
                    if NAME.fullmatch(entry.name) and entry.name.endswith('.wav'):
                        self.publish(entry.name)
                        count += 1
            return count

    def _build(self, source, name, work, frames, source_info, publication_id):
        basename = name[:-4] + '-' + publication_id
        outputs = {}
        channels = []
        source.seek(0)
        with wave.open(source, 'rb') as input_wav, contextlib.ExitStack() as stack:
            writers = []
            for channel in (1, 2):
                blob = 'TX%d.blob' % channel
                fd = os.open(blob, os.O_WRONLY | os.O_CREAT | os.O_EXCL | NOFOLLOW,
                             0o640, dir_fd=work)
                stream = stack.enter_context(os.fdopen(fd, 'wb'))
                writer = stack.enter_context(wave.open(stream, 'wb'))
                writer.setparams((1, 2, 16000, frames, 'NONE', 'not compressed'))
                writers.append((writer, stream))
            pcm_hashes = [hashlib.sha256(), hashlib.sha256()]
            left = frames
            while left:
                data = input_wav.readframes(min(CHUNK // 4, left))
                if not data or len(data) % 4:
                    raise ReceiverError(422)
                for index, (writer, _) in enumerate(writers):
                    mono = bytearray(len(data) // 2)
                    mono[0::2] = data[index * 2::4]
                    mono[1::2] = data[index * 2 + 1::4]
                    writer.writeframesraw(mono)
                    pcm_hashes[index].update(mono)
                left -= len(data) // 4
            # Finalize headers before flushing/fsync; explicit close is idempotent.
            for writer, stream in writers:
                writer.close()
                stream.flush()
                os.fsync(stream.fileno())
        for channel in (1, 2):
            blob = 'TX%d.blob' % channel
            with regular(work, blob) as stream:
                info = digest_file(stream)
            final = basename + '-TX%d.wav' % channel
            outputs[blob] = {'name': final, **info}
            channels.append({'identity': 'TX%d' % channel, 'input_channel': channel,
                             'file': final, 'wav_sha256': info['sha256'],
                             'pcm_sha256': pcm_hashes[channel - 1].hexdigest()})
        source.seek(0)
        atomic_private(work, 'source.blob', lambda target: shutil.copyfileobj(source, target, CHUNK))
        outputs['source.blob'] = {'name': basename + '-stereo.wav.source', **source_info}
        provenance = {'schema': 1, 'publication_id': publication_id, 'source_name': name,
                      'source_sha256': source_info['sha256'], 'source_bytes': source_info['bytes'],
                      'frames': frames, 'sample_rate': 16000, 'sample_width_bytes': 2,
                      'channels': channels, 'transcription': 'not performed'}
        atomic_private(work, 'provenance.blob', lambda target: target.write(canonical_json(provenance)))
        with regular(work, 'provenance.blob') as stream:
            outputs['provenance.blob'] = {'name': basename + '.json', **digest_file(stream)}
        return {'schema': 1, 'publication_id': publication_id, 'source': source_info,
                'source_name': name, 'files': outputs}

    def publish(self, name):
        """All validation precedes visibility. Durable manifest enables partial retry.

        Files become visible individually (not an impossible multi-file rename).
        A retry verifies *every* existing final before linking any missing final.
        """
        with self.publish_lock, regular(self.inbox, name) as source:
            frames = validate_wav(source, self.max_bytes)
            source_info = digest_file(source)
            self.require_space(self.output, source_info['bytes'] * 2 + CHUNK)
            publication_id = hashlib.sha256((name + '\0' + source_info['sha256']).encode('ascii')).hexdigest()
            transaction = child_directory(self.staging, publication_id)
            work_name = '.work-' + uuid.uuid4().hex
            work = child_directory(transaction, work_name)
            try:
                manifest = self._build(source, name, work, frames, source_info, publication_id)
                # Regenerate deterministically, then compare to the persisted journal.
                try:
                    with regular(transaction, 'manifest.json') as stream:
                        saved = stream.read(65537)
                    if len(saved) > 65536 or saved != canonical_json(manifest):
                        raise ReceiverError(409)
                    for blob, info in manifest['files'].items():
                        if not file_matches(transaction, blob, {k: info[k] for k in ('sha256', 'bytes')}):
                            raise ReceiverError(409)
                except FileNotFoundError:
                    for blob in manifest['files']:
                        os.replace(blob, blob, src_dir_fd=work, dst_dir_fd=transaction)
                    os.fsync(transaction)
                    atomic_private(transaction, 'manifest.json',
                                   lambda target: target.write(canonical_json(manifest)))
                # Preflight ALL collisions before making any new file visible.
                for info in manifest['files'].values():
                    file_matches(self.output, info['name'], {k: info[k] for k in ('sha256', 'bytes')})
                # Source + provenance first; complete validated mono files last.
                order = ('source.blob', 'provenance.blob', 'TX1.blob', 'TX2.blob')
                for blob in order:
                    info = manifest['files'][blob]
                    expected = {k: info[k] for k in ('sha256', 'bytes')}
                    if not file_matches(self.output, info['name'], expected):
                        os.link(blob, info['name'], src_dir_fd=transaction, dst_dir_fd=self.output,
                                follow_symlinks=False)
                        os.fsync(self.output)
                for info in manifest['files'].values():
                    if not file_matches(self.output, info['name'], {k: info[k] for k in ('sha256', 'bytes')}):
                        raise ReceiverError(409)
            finally:
                with os.scandir(work) as entries:
                    for entry in entries:
                        os.unlink(entry.name, dir_fd=work)
                os.close(work)
                os.rmdir(work_name, dir_fd=transaction)
                os.close(transaction)


def parse_path(raw, collection=False):
    if not raw.startswith('/inbox') or raw.startswith('//') or '?' in raw or '#' in raw:
        raise ReceiverError(400)
    if re.search(r'%(?![0-9A-Fa-f]{2})', raw):
        raise ReceiverError(400)
    path = unquote(raw, encoding='utf-8', errors='strict')
    if '%' in path or '\\' in path or '..' in path:
        raise ReceiverError(400)
    if collection and path in ('/inbox', '/inbox/'):
        return None
    if not path.startswith('/inbox/') or not NAME.fullmatch(path[7:]):
        raise ReceiverError(400)
    return path[7:]


def configured_origin(value):
    parsed = urlsplit(value)
    if (parsed.scheme != 'http' or not parsed.hostname or parsed.username is not None
            or parsed.password is not None or parsed.path or parsed.query or parsed.fragment
            or parsed.port is None or parsed.netloc != '%s:%s' % (parsed.hostname, parsed.port)):
        raise ValueError('origin must be exact http://hostname:port (IPv4 or DNS), no path')
    return value


class Handler(BaseHTTPRequestHandler):
    server: 'Server'
    protocol_version = 'HTTP/1.1'

    def setup(self):
        self.request.settimeout(self.server.app.timeout)
        super().setup()
        self.header_timer = threading.Timer(self.server.app.header_timeout, self.expire_headers)
        self.header_timer.daemon = True
        self.header_timer.start()

    def expire_headers(self):
        with contextlib.suppress(OSError):
            self.connection.shutdown(socket.SHUT_RDWR)

    def parse_request(self):
        try:
            return super().parse_request()
        finally:
            self.header_timer.cancel()

    def finish(self):
        self.header_timer.cancel()
        self.header_timer.join()
        super().finish()

    def log_message(self, format, *args):
        pass  # No credentials, payloads, raw paths or headers in logs.

    def send_error(self, code, *_args, **_kwargs):
        self.reply(code)

    def reply(self, status, size=0):
        self.send_response(status)
        self.send_header('Content-Length', str(size))
        self.send_header('Connection', 'close')
        if status == 401:
            self.send_header('WWW-Authenticate', 'Basic realm="gencord"')
        self.end_headers()
        self.close_connection = True

    def one_header(self, name, required=False):
        values = self.headers.get_all(name, [])
        if len(values) > 1 or (required and len(values) != 1):
            raise ReceiverError(400)
        return values[0] if values else None

    def dispatch(self):
        try:
            auth = self.one_header('Authorization') or ''
            if not hmac.compare_digest(auth.encode('utf-8'), self.server.authorization):
                raise ReceiverError(401)
            if self.command not in ('HEAD', 'PUT', 'MOVE', 'MKCOL'):
                raise ReceiverError(405)
            if self.one_header('Transfer-Encoding') is not None or self.one_header('Expect') is not None:
                raise ReceiverError(400)
            length = self.one_header('Content-Length')
            if self.command == 'PUT':
                if length is None:
                    raise ReceiverError(411)
                if not re.fullmatch(r'[0-9]{1,12}', length) or int(length) == 0:
                    raise ReceiverError(400)
                if int(length) > self.server.app.max_bytes:
                    raise ReceiverError(413)
            elif length is not None and length != '0':
                raise ReceiverError(400)
            # BaseHTTPRequestHandler normalizes leading //; validate the original target.
            raw_target = self.requestline.split()[1]
            name = parse_path(raw_target, collection=self.command == 'MKCOL')
            app = self.server.app
            with app.lock:
                if self.command == 'MKCOL':
                    if name is not None:
                        raise ReceiverError(405)
                    return self.reply(405)  # Precreated collection: firmware accepts this.
                if self.command == 'HEAD':
                    try:
                        with regular(app.inbox, name) as stream:
                            size = os.fstat(stream.fileno()).st_size
                    except FileNotFoundError:
                        raise ReceiverError(404) from None
                    return self.reply(200, size)
                if self.command == 'PUT':
                    if not name.endswith('.wav.part'):
                        raise ReceiverError(405)
                    return self.reply(app.put(name, self.rfile, int(length), self.connection))
                destination = self.one_header('Destination', required=True)
                # Compare the configured absolute origin, NEVER the request Host.
                prefix = self.server.origin + '/'
                if not destination.startswith(prefix):
                    raise ReceiverError(400)
                target = parse_path(destination[len(self.server.origin):])
                if not name.endswith('.wav.part') or target != name[:-5]:
                    raise ReceiverError(400)
                overwrite = self.one_header('Overwrite')
                if overwrite not in (None, 'T', 'F'):
                    raise ReceiverError(400)
                return self.reply(app.move(name, target, defer=self.server.background))
        except ReceiverError as error:
            self.reply(error.status)
        except (TimeoutError, socket.timeout):
            self.reply(408)
        except FileNotFoundError:
            self.reply(404)
        except (ValueError, UnicodeError):
            self.reply(400)
        except (OSError, wave.Error):
            self.reply(503)

    def handle_expect_100(self):
        self.reply(400)
        return False

    do_HEAD = do_PUT = do_MOVE = do_MKCOL = dispatch
    do_GET = do_DELETE = do_PROPFIND = do_POST = do_OPTIONS = dispatch


class Server(HTTPServer):
    app: Receiver
    origin: str
    authorization: bytes
    background: bool = False
    # Deliberately single-worker: bounded memory + global request serialization.
    allow_reuse_address = True

    def handle_error(self, request, client_address):
        # Do not let stdlib print raw request context or exception messages.
        print('receiver request failed', flush=True)

    def server_close(self):
        if self.background:
            self.app.stop_worker()
        super().server_close()


def make_server(app, address, username, password, origin=None, background=False):
    if not username or ':' in username or not password or any(c in username + password for c in '\r\n'):
        raise ValueError('nonempty Basic credentials required')
    server = Server(address, Handler)
    try:
        server.app = app
        server.origin = configured_origin(origin or 'http://%s:%s' % server.server_address)
        server.authorization = b'Basic ' + base64.b64encode((username + ':' + password).encode('utf-8'))
        server.background = background
        if background:
            app.start_worker()
        return server
    except BaseException:
        server.server_close()
        raise


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--state', required=True)
    parser.add_argument('--recordings-root', required=True, help='existing recordings root, NOT its cardputer child')
    parser.add_argument('--bind', default='127.0.0.1')
    parser.add_argument('--port', type=int, default=8795)
    parser.add_argument('--origin', help='exact firmware endpoint origin, e.g. http://192.168.1.102:8795')
    parser.add_argument('--max-bytes', type=int, default=MAX_BYTES)
    parser.add_argument('--timeout', type=float, default=20)
    parser.add_argument('--header-timeout', type=float, default=5)
    parser.add_argument('--body-timeout', type=float, default=600)
    parser.add_argument('--max-inbox-bytes', type=int, default=4 * 1024**3)
    parser.add_argument('--max-files', type=int, default=1024)
    parser.add_argument('--reserve-bytes', type=int, default=256 * 1024**2)
    parser.add_argument('--check', action='store_true', help='check configuration/directories/locks; creates owned directories')
    parser.add_argument('--import-once', action='store_true', help='publish complete inbox WAVs, then exit')
    args = parser.parse_args()
    username = password = ''
    try:
        if not (args.check or args.import_once):
            if args.origin is None:
                parser.error('--origin is required for service mode')
            configured_origin(args.origin)
            username = os.environ.get('GENCORD_USER', '')
            password = os.environ.get('GENCORD_PASSWORD', '')
            if not username or not password:
                parser.error('GENCORD_USER and GENCORD_PASSWORD must be set')
        app = Receiver(args.state, args.recordings_root, args.max_bytes, args.timeout,
                       args.header_timeout, args.body_timeout, args.max_inbox_bytes,
                       args.max_files, args.reserve_bytes)
        try:
            if args.check:
                print('configuration, directory confinement and exclusive locks OK (no listener opened)')
            elif args.import_once:
                print('verified publications:', app.import_once())
            else:
                # Worker recovers durable intake on startup; listener never waits for splitting.
                with make_server(app, (args.bind, args.port), username, password,
                                 args.origin, background=True) as server:
                    print('receiver ready; transcription not performed', flush=True)
                    server.serve_forever()
        finally:
            app.close()
    except (ReceiverError, OSError, ValueError, wave.Error):
        parser.exit(1, 'receiver refused configuration, storage, lock or publication; inspect offline\n')
    except KeyboardInterrupt:
        return


if __name__ == '__main__':
    main()
