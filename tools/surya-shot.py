#!/usr/bin/env python3
"""Screenshot of the phone's panel memory (cont_splash 0x9c000000, BGRA) over the USB file server.

  surya-shot.py OUT.png [--stride BYTES]

With the GPU present (sweetgpu=1) the panel rows are 4352 bytes, else 4320 (1080 x 4).
"""
import os, struct, subprocess, sys, tempfile, zlib

FB, W, H = 0x9c000000, 1080, 2400


def png(path, w, h, rows):
    def chunk(t, d):
        c = struct.pack('>I', len(d)) + t + d
        return c + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
    raw = b''.join(b'\0' + r for r in rows)
    with open(path, 'wb') as f:
        f.write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)) +
                chunk(b'IDAT', zlib.compress(raw, 6)) + chunk(b'IEND', b''))


def main(argv):
    if not argv:
        raise SystemExit(__doc__)
    out, stride = argv[0], 4352
    if '--stride' in argv:
        stride = int(argv[argv.index('--stride') + 1])
    tool = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'surya-files.py')
    with tempfile.TemporaryDirectory() as d:
        raw = os.path.join(d, 'fb')
        subprocess.run([sys.executable, tool, 'read', '/dev/pmem', hex(FB), str(stride * H), raw], check=True,
                       capture_output=True)
        data = open(raw, 'rb').read()
    rows = []
    for y in range(H):
        r = data[y * stride:y * stride + W * 4]
        rgb = bytearray(W * 3)
        rgb[0::3] = r[2::4]
        rgb[1::3] = r[1::4]
        rgb[2::3] = r[0::4]
        rows.append(bytes(rgb))
    png(out, W, H, rows)
    print(out)


if __name__ == '__main__':
    main(sys.argv[1:])
