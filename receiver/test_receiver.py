"""Real loopback HTTP and PCM WAV acceptance tests; no production paths."""
import base64
import concurrent.futures
import hashlib
import http.client
import io
import json
import os
from pathlib import Path
import socket
import select
import time
import struct
import subprocess
import sys
import tempfile
import threading
import unittest
from unittest import mock
import wave

from gencord_receiver import Receiver, make_server, ReceiverError


def wav_bytes(channels=2, frames=20000, rate=16000, width=2):
    stream = io.BytesIO()
    with wave.open(stream, 'wb') as w:
        w.setparams((channels, width, rate, 0, 'NONE', 'not compressed'))
        if width == 2:
            samples = [((i * (c + 3) * 29) % 65536) - 32768
                       for i in range(frames) for c in range(channels)]
            w.writeframes(struct.pack('<' + 'h' * len(samples), *samples))
        else:
            w.writeframes(b'\x11' * frames * channels * width)
    return stream.getvalue()


class ReceiverTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix='gencord-test-')
        self.root = Path(self.tmp.name)
        self.app = Receiver(self.root / 'state', self.root / 'recordings',
                            max_bytes=1024 * 1024, timeout=0.3)
        self.server = make_server(self.app, ('127.0.0.1', 0), 'fixture', 'fixture-only')
        self.origin = self.server.origin
        self.thread = threading.Thread(target=self.server.serve_forever, kwargs={'poll_interval': .01})
        self.thread.start()
        self.audio = wav_bytes()
        self.auth = 'Basic ' + base64.b64encode(b'fixture:fixture-only').decode()
        self.out = self.root / 'recordings' / 'cardputer'

    def tearDown(self):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join()
        self.app.close()
        self.tmp.cleanup()

    def request(self, method, path='/inbox/capture.wav.part', body=None, headers=None, auth=True):
        h = {'Authorization': self.auth} if auth else {}
        h.update(headers or {})
        conn = http.client.HTTPConnection(*self.server.server_address, timeout=5)
        conn.request(method, path, body=body, headers=h)
        resp = conn.getresponse()
        result = resp.status, dict(resp.getheaders()), resp.read()
        conn.close()
        return result

    def raw(self, text, body=b''):
        with socket.create_connection(self.server.server_address, timeout=5) as sock:
            sock.sendall(text.encode() + body)
            sock.shutdown(socket.SHUT_WR)
            data = b''
            while True:
                part = sock.recv(65536)
                if not part:
                    break
                data += part
        return int(data.split(b' ', 2)[1])

    def upload(self, audio=None, stem='capture'):
        return self.request('PUT', '/inbox/' + stem + '.wav.part',
                            self.audio if audio is None else audio)[0]

    def move(self, stem='capture', extra=None):
        headers = {'Destination': self.origin + '/inbox/' + stem + '.wav', 'Overwrite': 'T'}
        headers.update(extra or {})
        return self.request('MOVE', '/inbox/' + stem + '.wav.part', b'', headers)[0]

    def test_end_to_end_exact_split_and_retained_source(self):
        self.assertEqual(self.request('MKCOL', '/inbox', b'')[0], 405)
        self.assertEqual(self.upload(), 201)
        self.assertEqual(self.request('HEAD')[1]['Content-Length'], str(len(self.audio)))
        self.assertEqual(self.move(), 201)
        self.assertEqual(self.request('HEAD', '/inbox/capture.wav')[1]['Content-Length'], str(len(self.audio)))
        self.assertEqual((self.root / 'state/inbox/capture.wav').read_bytes(), self.audio)
        files = sorted(p.name for p in self.out.glob('*.wav'))
        self.assertEqual(len(files), 2)
        expected_pcm = self.audio[44:]
        for channel in (1, 2):
            path = next(self.out.glob('*-TX%d.wav' % channel))
            with wave.open(str(path), 'rb') as w:
                self.assertEqual(w.getparams()[:4], (1, 2, 16000, 20000))
                actual = w.readframes(w.getnframes())
            expected = b''.join(expected_pcm[i:i+2] for i in range((channel-1)*2, len(expected_pcm), 4))
            self.assertEqual(actual, expected)
            self.assertEqual(hashlib.sha256(actual).hexdigest(), hashlib.sha256(expected).hexdigest())
        self.assertEqual(next(self.out.glob('*.source')).read_bytes(), self.audio)
        provenance = json.loads(next(self.out.glob('*.json')).read_text())
        self.assertEqual(provenance['source_sha256'], hashlib.sha256(self.audio).hexdigest())
        self.assertEqual([c['identity'] for c in provenance['channels']], ['TX1', 'TX2'])
        self.assertEqual([c['input_channel'] for c in provenance['channels']], [1, 2])
        self.assertFalse(list(self.out.glob('*.txt')))

    def test_auth_and_forbidden_methods_no_listing(self):
        self.assertEqual(self.request('PUT', body=self.audio, auth=False)[0], 401)
        self.assertEqual(self.request('HEAD', auth=False)[0], 401)
        self.assertEqual(self.request('PUT', body=self.audio, headers={'Authorization': 'Basic bad'})[0], 401)
        for method in ('GET', 'DELETE', 'PROPFIND', 'POST'):
            self.assertEqual(self.request(method, '/inbox', b'')[0], 405)
        self.assertFalse(list((self.root / 'state/inbox').iterdir()))

    def test_paths_decoded_once_and_confined(self):
        for path in ('/inbox/../evil.wav.part', '/inbox/%2e%2e/evil.wav.part',
                     '/inbox/%252e%252e.wav.part', '/inbox/a%2fb.wav.part',
                     '/inbox/a%5cb.wav.part', '/inbox/.hidden.wav.part',
                     '/inbox/a.wav.part?x=1', '//inbox/a.wav.part',
                     '/inbox/a.wav.part#x', '/inbox/a%00.wav.part',
                     '/inbox/a..b.wav.part', '/inbox/a%ZZ.wav.part',
                     'http://foreign/inbox/a.wav.part'):
            with self.subTest(path=path):
                self.assertEqual(self.request('PUT', path, self.audio)[0], 400)
        self.assertEqual(self.request('PUT', '/inbox/capture.wav', self.audio)[0], 405)

    def test_destination_origin_not_host_header(self):
        self.assertEqual(self.upload(), 201)
        for dest in ('http://foreign/inbox/capture.wav', '//foreign/inbox/capture.wav',
                     '/inbox/capture.wav', self.origin + '/inbox/%252e%252e.wav',
                     self.origin + '/inbox/../capture.wav', self.origin + '/inbox/other.wav',
                     self.origin + '/inbox/capture.wav?x=1',
                     self.origin.replace('http:', 'https:') + '/inbox/capture.wav',
                     self.origin.replace('127.0.0.1', 'localhost') + '/inbox/capture.wav',
                     self.origin.replace('http://', 'http://u@') + '/inbox/capture.wav'):
            with self.subTest(dest=dest):
                self.assertEqual(self.move(extra={'Destination': dest, 'Host': 'foreign'}), 400)
        self.assertEqual(self.move(extra={'Host': 'foreign'}), 201)

    def test_length_framing_limits(self):
        prefix = 'PUT /inbox/a.wav.part HTTP/1.1\r\nHost: fixture\r\nAuthorization: ' + self.auth + '\r\n'
        cases = [('Transfer-Encoding: chunked\r\n', 400), ('', 411),
                 ('Content-Length: 0\r\n', 400), ('Content-Length: -1\r\n', 400),
                 ('Content-Length: 1048577\r\n', 413),
                 ('Content-Length: 10\r\nContent-Length: 10\r\n', 400),
                 ('Content-Length: nope\r\n', 400)]
        for headers, status in cases:
            with self.subTest(headers=headers):
                self.assertEqual(self.raw(prefix + headers + '\r\n'), status)
        self.assertEqual(self.raw(prefix + 'Content-Length: 100\r\n\r\n', b'short'), 400)
        self.assertFalse(list((self.root / 'state/inbox').iterdir()))

    def test_timeout_does_not_commit(self):
        with socket.create_connection(self.server.server_address, timeout=5) as s:
            s.sendall(('PUT /inbox/a.wav.part HTTP/1.1\r\nAuthorization: ' + self.auth +
                       '\r\nContent-Length: 100\r\n\r\n').encode())
            self.assertIn(b'408', s.recv(4096))
        self.assertFalse(list((self.root / 'state/inbox').iterdir()))

    def test_only_canonical_stereo_pcm16_16000_valid(self):
        bad = [wav_bytes(channels=1), wav_bytes(rate=8000), wav_bytes(width=1),
               b'not a wave', self.audio[:-1], self.audio + b'junk', wav_bytes(frames=0)]
        torn = bytearray(self.audio[:-2])
        struct.pack_into('<I', torn, 4, len(torn)-8)
        struct.pack_into('<I', torn, 40, len(torn)-44)
        bad.append(bytes(torn))
        for audio in bad:
            with self.subTest(size=len(audio)):
                self.assertEqual(self.upload(audio), 422)
        self.assertFalse(list((self.root / 'state/inbox').iterdir()))
        self.assertFalse(list(self.out.glob('*.wav')))

    def test_same_path_retries_and_collision(self):
        self.assertEqual(self.upload(), 201)
        self.assertEqual(self.upload(), 204)
        other = wav_bytes(frames=20001)
        self.assertEqual(self.upload(other), 409)
        self.assertEqual(self.move(), 201)
        self.assertEqual(self.move(), 204)
        self.assertEqual(self.upload(), 201)
        self.assertEqual(self.move(), 204)
        self.assertEqual(self.upload(other), 201)
        self.assertEqual(self.move(), 409)
        self.assertEqual((self.root / 'state/inbox/capture.wav').read_bytes(), self.audio)
        self.assertEqual(len(list(self.out.glob('*.wav'))), 2)

    def test_concurrent_put_same_path_serialized(self):
        payloads = [self.audio, self.audio, wav_bytes(frames=20001)]
        with concurrent.futures.ThreadPoolExecutor(3) as pool:
            statuses = list(pool.map(self.upload, payloads))
        self.assertEqual(statuses.count(201), 1)
        self.assertTrue(all(s in (201, 204, 409) for s in statuses))
        winner = (self.root / 'state/inbox/capture.wav.part').read_bytes()
        self.assertIn(winner, payloads)
        self.assertEqual(self.move(), 201)

    def test_symlink_inbox_leaf_rejected(self):
        outside = self.root / 'outside'
        outside.write_bytes(b'protected')
        leaf = self.root / 'state/inbox/capture.wav.part'
        leaf.symlink_to(outside)
        self.assertEqual(self.upload(), 409)
        self.assertEqual(self.request('HEAD')[0], 409)
        self.assertEqual(self.move(), 409)
        self.assertEqual(outside.read_bytes(), b'protected')

    def test_output_collision_no_partial_new_channels(self):
        self.assertEqual(self.upload(), 201)
        # Learn deterministic output names via a separate valid source, then replay.
        self.assertEqual(self.move(), 201)
        tx1 = next(self.out.glob('*-TX1.wav'))
        tx2 = next(self.out.glob('*-TX2.wav'))
        tx1.unlink()
        tx2.unlink()
        tx2.write_bytes(b'unrelated recording')
        self.assertEqual(self.move(), 503)
        self.assertFalse(tx1.exists())
        self.assertEqual(tx2.read_bytes(), b'unrelated recording')

    def test_output_symlink_never_followed(self):
        self.assertEqual(self.upload(), 201)
        self.assertEqual(self.move(), 201)
        tx1 = next(self.out.glob('*-TX1.wav'))
        tx1.unlink()
        outside = self.root / 'outside'
        outside.write_bytes(b'protected')
        tx1.symlink_to(outside)
        self.assertEqual(self.move(), 503)
        self.assertEqual(outside.read_bytes(), b'protected')

    def test_failed_partial_publish_is_retryable(self):
        self.assertEqual(self.upload(), 201)
        original = os.link
        count = [0]
        def fail_second_channel(src, dst, *args, **kwargs):
            if str(dst).endswith('-TX2.wav'):
                count[0] += 1
                if count[0] == 1:
                    raise OSError('simulated power-loss boundary')
            return original(src, dst, *args, **kwargs)
        with mock.patch('gencord_receiver.os.link', side_effect=fail_second_channel):
            self.assertEqual(self.move(), 503)
        self.assertEqual(len(list(self.out.glob('*.wav'))), 1)
        self.assertEqual(self.move(), 204)
        self.assertEqual(len(list(self.out.glob('*.wav'))), 2)
        self.assertEqual(self.app.import_once(), 1)

    def test_import_only_complete_moved_and_restart(self):
        self.assertEqual(self.upload(), 201)
        self.assertEqual(self.app.import_once(), 0)
        self.assertFalse(list(self.out.glob('*.wav')))
        self.assertEqual(self.move(), 201)
        self.server.shutdown()
        self.server.server_close()
        self.thread.join()
        self.app.close()
        self.app = Receiver(self.root/'state', self.root/'recordings', max_bytes=1024*1024)
        self.assertEqual(self.app.import_once(), 1)
        self.assertEqual(len(list(self.out.glob('*.wav'))), 2)

    def test_private_staging_permissions(self):
        os.chmod(self.out / '.gencord-staging', 0o755)
        self.app.close()
        self.app = Receiver(self.root/'state', self.root/'recordings')
        self.assertEqual((self.out / '.gencord-staging').stat().st_mode & 0o777, 0o700)

    def test_overlapping_state_and_recordings_refused_before_chmod(self):
        unrelated = self.root / 'unrelated'
        unrelated.mkdir(mode=0o755)
        with self.assertRaises((ReceiverError, ValueError)):
            Receiver(unrelated, unrelated)
        self.assertEqual(unrelated.stat().st_mode & 0o777, 0o755)

    def test_partial_publish_verified_after_restart(self):
        self.assertEqual(self.upload(), 201)
        original = os.link
        def fail_second_channel(src, dst, *args, **kwargs):
            if str(dst).endswith('-TX2.wav'):
                raise OSError('simulated interrupted publish')
            return original(src, dst, *args, **kwargs)
        with mock.patch('gencord_receiver.os.link', side_effect=fail_second_channel):
            self.assertEqual(self.move(), 503)
        first = next(self.out.glob('*-TX1.wav')).read_bytes()
        self.server.shutdown()
        self.server.server_close()
        self.thread.join()
        self.app.close()
        self.app = Receiver(self.root/'state', self.root/'recordings')
        self.assertEqual(self.app.import_once(), 1)
        self.assertEqual(next(self.out.glob('*-TX1.wav')).read_bytes(), first)
        self.assertEqual(len(list(self.out.glob('*.wav'))), 2)

    def test_corrupt_journal_fails_closed(self):
        self.assertEqual(self.upload(), 201)
        self.assertEqual(self.move(), 201)
        manifest = next((self.out / '.gencord-staging').glob('*/manifest.json'))
        manifest.write_bytes(b'{"corrupted":true}\n')
        before = {p.name: p.read_bytes() for p in self.out.glob('*') if p.is_file()}
        self.assertEqual(self.move(), 503)
        self.assertEqual({p.name: p.read_bytes() for p in self.out.glob('*') if p.is_file()}, before)

    def test_corrupt_private_blob_fails_closed(self):
        self.assertEqual(self.upload(), 201)
        self.assertEqual(self.move(), 201)
        blob = next((self.out / '.gencord-staging').glob('*/TX1.blob'))
        blob.unlink()
        blob.write_bytes(b'wrong staged bytes')
        next(self.out.glob('*-TX2.wav')).unlink()
        self.assertEqual(self.move(), 503)
        self.assertFalse(list(self.out.glob('*-TX2.wav')))

    def test_header_deadline_stops_trickle(self):
        self.app.header_timeout = .15
        start = time.monotonic()
        with socket.create_connection(self.server.server_address, timeout=3) as sock:
            sock.sendall(b'PUT /inbox/a.wav.part HTTP/1.1\r\nX-Trickle: ')
            closed = False
            for _ in range(20):
                try:
                    sock.sendall(b'x')
                    time.sleep(.04)
                    readable, _, _ = select.select([sock], [], [], 0)
                    if readable:
                        closed = not sock.recv(4096)
                        break
                except OSError:
                    closed = True
                    break
        self.assertTrue(closed)
        self.assertLess(time.monotonic() - start, .6)

    def test_body_deadline_distinct_from_inactivity(self):
        self.app.body_timeout = .15
        with socket.create_connection(self.server.server_address, timeout=3) as sock:
            sock.sendall(('PUT /inbox/a.wav.part HTTP/1.1\r\nAuthorization: ' + self.auth +
                          '\r\nContent-Length: 1000\r\n\r\n').encode())
            response = b''
            for _ in range(20):
                try:
                    sock.sendall(b'x')
                except OSError:
                    break
                time.sleep(.04)
                readable, _, _ = select.select([sock], [], [], 0)
                if readable:
                    response = sock.recv(4096)
                    break
        self.assertIn(b'408', response)
        self.assertFalse(list((self.root / 'state/inbox').iterdir()))

    def test_inbox_byte_budget(self):
        self.app.max_inbox_bytes = len(self.audio) - 1
        self.assertEqual(self.upload(), 507)
        self.assertFalse(list((self.root / 'state/inbox').iterdir()))

    def test_inbox_file_budget(self):
        self.app.max_files = 1
        self.assertEqual(self.upload(), 201)
        self.assertEqual(self.upload(stem='second'), 507)

    def test_async_move_returns_before_publish_and_head_works(self):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join()
        self.server = make_server(self.app, ('127.0.0.1', 0), 'fixture', 'fixture-only', background=True)
        self.origin = self.server.origin
        self.thread = threading.Thread(target=self.server.serve_forever, kwargs={'poll_interval': .01})
        self.thread.start()
        entered, release = threading.Event(), threading.Event()
        actual_publish = self.app.publish
        def slow_publish(name):
            entered.set()
            if not release.wait(3):
                raise RuntimeError('test worker was not released')
            return actual_publish(name)
        with mock.patch.object(self.app, 'publish', side_effect=slow_publish):
            try:
                self.assertEqual(self.upload(), 201)
                start = time.monotonic()
                self.assertEqual(self.move(), 201)
                self.assertLess(time.monotonic() - start, 1)
                self.assertTrue(entered.wait(1))
                self.assertEqual(self.request('HEAD', '/inbox/capture.wav')[0], 200)
                self.assertEqual(self.move(), 204)
                release.set()
                self.assertTrue(self.app.wait_idle(3))
            finally:
                release.set()
        self.assertEqual(len(list(self.out.glob('*.wav'))), 2)
        self.assertEqual((self.root/'state/inbox/capture.wav').read_bytes(), self.audio)

    def test_setgid_output_preserved_for_syncthing(self):
        if os.geteuid() != 0:
            self.skipTest('group ownership fixture needs root; no live paths used')
        self.app.close()
        staging = self.out / '.gencord-staging'
        (staging / '.lock').unlink()
        staging.rmdir()
        os.chown(self.out, os.getuid(), 33)
        os.chmod(self.out, 0o2770)
        self.app = Receiver(self.root/'state', self.root/'recordings')
        self.assertEqual(self.upload(), 201)
        self.assertEqual(self.move(), 201)
        self.assertEqual(next(self.out.glob('*-TX1.wav')).stat().st_gid, 33)
        self.assertEqual(next(self.out.glob('*.source')).stat().st_gid, 33)
        self.assertTrue((self.out / '.gencord-staging').stat().st_mode & 0o2000)

    def test_importer_refuses_manually_corrupted_ready_files(self):
        for audio in (wav_bytes(channels=1), self.audio[:-2], b'not wave'):
            path = self.root / 'state/inbox/manual.wav'
            path.write_bytes(audio)
            with self.assertRaises(ReceiverError):
                self.app.import_once()
            self.assertEqual(path.read_bytes(), audio)
            self.assertFalse(list(self.out.glob('*.wav')))
            path.unlink()

    def test_free_space_reserve_refuses_before_write(self):
        with mock.patch('gencord_receiver.os.fstatvfs') as stats:
            stats.return_value.f_bavail = 1
            stats.return_value.f_frsize = 4096
            self.assertEqual(self.upload(), 507)
        self.assertFalse(list((self.root / 'state/inbox').iterdir()))

    def test_cli_check_and_import_once(self):
        state = self.root / 'cli-state'
        recordings = self.root / 'cli-recordings'
        command = [sys.executable, str(Path(__file__).with_name('gencord_receiver.py')),
                   '--state', str(state), '--recordings-root', str(recordings)]
        checked = subprocess.run(command + ['--check'], capture_output=True, timeout=10)
        self.assertEqual(checked.returncode, 0, checked.stderr)
        self.assertIn(b'no listener opened', checked.stdout)
        original = state / 'inbox/cli.wav'
        original.write_bytes(self.audio)
        imported = subprocess.run(command + ['--import-once'], capture_output=True, timeout=10)
        self.assertEqual(imported.returncode, 0, imported.stderr)
        self.assertIn(b'verified publications: 1', imported.stdout)
        self.assertEqual(len(list((recordings/'cardputer').glob('*.wav'))), 2)
        self.assertEqual(original.read_bytes(), self.audio)

    def test_real_unprivileged_nonmember_setgid_publication(self):
        if os.geteuid() != 0:
            self.skipTest('privilege-drop fixture requires root')
        # Dedicated temporary directory, never a real account/service/path change.
        with tempfile.TemporaryDirectory(prefix='gencord-uid-') as temporary:
            root = Path(temporary)
            os.chown(root, 65534, 65534)
            recordings = root / 'recordings'
            recordings.mkdir()
            os.chown(recordings, 65534, 65534)
            output = recordings / 'cardputer'
            output.mkdir()
            os.chown(output, 65534, 33)
            os.chmod(output, 0o2770)
            # Service has traversal only on the unrelated recordings root.
            os.chown(recordings, 0, 0)
            os.chmod(recordings, 0o711)
            code = '''import json, os, pathlib, sys
from gencord_receiver import Receiver
os.setgroups([])
os.setgid(65534)
os.setuid(65534)
root = pathlib.Path(sys.argv[1])
app = Receiver(root/'state', root/'recordings')
(root/'state/inbox/uid.wav').write_bytes(sys.stdin.buffer.read())
app.import_once()
out = root/'recordings/cardputer'
print(json.dumps({'uid': os.getuid(), 'groups': os.getgroups(),
                  'output_gid': next(out.glob('*-TX1.wav')).stat().st_gid,
                  'private_mode': (out/'.gencord-staging').stat().st_mode & 0o7777}))
app.close()
'''
            child = subprocess.run([sys.executable, '-c', code, str(root)], input=self.audio,
                                   capture_output=True, timeout=10)
            self.assertEqual(child.returncode, 0, child.stderr)
            report = json.loads(child.stdout)
            self.assertEqual(report, {'uid': 65534, 'groups': [], 'output_gid': 33,
                                      'private_mode': 0o2700})

    def test_async_failure_retains_source_then_move_retries(self):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join()
        self.server = make_server(self.app, ('127.0.0.1', 0), 'fixture', 'fixture-only', background=True)
        self.origin = self.server.origin
        self.thread = threading.Thread(target=self.server.serve_forever, kwargs={'poll_interval': .01})
        self.thread.start()
        with mock.patch.object(self.app, 'publish', side_effect=ReceiverError(409)):
            self.assertEqual(self.upload(), 201)
            self.assertEqual(self.move(), 201)  # durable intake, NOT successful publication
            self.assertTrue(self.app.wait_idle(3))
        self.assertFalse(list(self.out.glob('*.wav')))
        self.assertEqual((self.root/'state/inbox/capture.wav').read_bytes(), self.audio)
        self.assertEqual(self.move(), 204)
        self.assertTrue(self.app.wait_idle(3))
        self.assertEqual(len(list(self.out.glob('*.wav'))), 2)

    def test_process_lock_excludes_second_instance(self):
        with self.assertRaises((ReceiverError, OSError)):
            Receiver(self.root/'state', self.root/'recordings')

    def test_symlink_directory_rejected(self):
        (self.root / 'linked').symlink_to(self.root / 'state', target_is_directory=True)
        with self.assertRaises((ReceiverError, OSError)):
            Receiver(self.root/'linked', self.root/'more-recordings')


if __name__ == '__main__':
    unittest.main(verbosity=2)
