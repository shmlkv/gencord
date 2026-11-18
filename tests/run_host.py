#!/usr/bin/env python3
"""Offline host behavior tests only; never opens a physical device or network."""
from pathlib import Path
import os
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / '.build-tools/host-tests'
OUT.mkdir(parents=True, exist_ok=True)
os.environ['TMPDIR'] = str(ROOT / '.build-tools/tmp')
common = ['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror', '-pthread',
          '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-g',
          '-Isrc', '-Itests/fakes', '-I' + str(OUT)]
# Extract, don't reimplement: exercise the exact current vendored C callback.
source = (ROOT / 'lib/usb_host_uac/src/uac_host.c').read_text()
start = source.index('static void stream_rx_xfer_done(')
end = source.index('static uint32_t _uac_fill_tx_xfer_packets', start)
(OUT / 'actual_uac_rx.inc').write_text(source[start:end])
suites = [
    ('pcm', ['tests/pcm_pipeline_test.cpp', 'src/pcm_pipeline.cpp']),
    ('capture', ['tests/usb_capture_test.cpp', 'src/usb_mic.cpp', 'src/pcm_pipeline.cpp']),
    ('fifo', ['tests/usb_fifo_test.cpp', 'src/usb_rx_fifo.cpp']),
    ('uac', ['tests/uac_rx_test.cpp']),
]
for name, files in suites:
    if len(sys.argv) > 1 and name not in sys.argv[1:]:
        continue
    binary = OUT / name
    print('BUILD/RUN', name, flush=True)
    subprocess.run(common + files + ['-o', str(binary)], cwd=ROOT, check=True)
    subprocess.run([str(binary)], cwd=ROOT, check=True)
print('ALL SELECTED HOST SUITES PASSED', flush=True)
