"""Seed files for tests/fuzz: small valid files of each format, from the samples of the system and made here.
usage: python3 -I tests/fuzz/seeds.py OUT_DIR"""
import gzip
import io
import lzma
import os
import shutil
import struct
import sys
import tarfile
import zipfile
import zlib

root = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
out = sys.argv[1]


def put(target, name, data):
    os.makedirs(os.path.join(out, target), exist_ok=True)
    with open(os.path.join(out, target, name), 'wb') as f:
        f.write(data)


def copy(target, rel):
    with open(os.path.join(root, rel), 'rb') as f:
        put(target, os.path.basename(rel), f.read())


if os.path.isdir(out):
    shutil.rmtree(out)

# PE: a 32-bit DLL with exports and imports, a 64-bit program
copy('pe', 'programs/win32/prebuilt/greet.dll')
copy('pe', 'programs/win64/prebuilt/hello64u.exe')

# pictures of the kernel library
try:
    from PIL import Image
    img = Image.new('RGBA', (9, 7))
    for y in range(7):
        for x in range(9):
            img.putpixel((x, y), (x * 28, y * 36, 90, 128 + x * 10))
    for fmt, name, mode, extra in (('PNG', 'a.png', 'RGBA', {}), ('PNG', 'p.png', 'P', {}),
                                   ('JPEG', 'a.jpg', 'RGB', {'quality': 70}),
                                   ('JPEG', 'p.jpg', 'RGB', {'progressive': True}), ('GIF', 'a.gif', 'P', {}),
                                   ('BMP', 'a.bmp', 'RGB', {})):
        b = io.BytesIO()
        img.convert(mode).save(b, fmt, **extra)
        put('picture', name, b.getvalue())
except ImportError:
    print('fuzz: no PIL, the picture target starts from nothing')

# the codecs module
for f in ('Deflate.tif', 'Gradient.tif', 'Gradient.pgm', 'Gradient.qoi', 'Gradient.tga', 'Icon.ico',
          'Tone.flac', 'Tone.aiff'):
    copy('codecs', 'rootfs/system/samples/Formats/' + f)

# the media module
for f in ('Gradient.webp', 'Lossless.webp', 'Tone.mp3', 'Tone.ogg'):
    copy('media', 'rootfs/system/samples/Formats/' + f)

# documents
for f in ('Welcome.pdf', 'Welcome.docx', 'Budget.xlsx'):
    copy('doc', 'rootfs/system/samples/' + f)
objs = [b'<< /Type /Catalog /Pages 2 0 R >>', b'<< /Type /Pages /Kids [3 0 R] /Count 1 >>',
        b'<< /Type /Page /Parent 2 0 R /MediaBox [0 0 200 100] /Contents 4 0 R '
        b'/Resources << /Font << /F1 5 0 R >> >> >>', None,
        b'<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>']
content = zlib.compress(b'BT /F1 12 Tf 10 50 Td (Hello) Tj ET 0 0 1 rg 10 10 50 20 re f')
objs[3] = b'<< /Length %d /Filter /FlateDecode >>\nstream\n' % len(content) + content + b'\nendstream'
pdf, offsets = b'%PDF-1.4\n', []
for i, o in enumerate(objs):
    offsets.append(len(pdf))
    pdf += b'%d 0 obj\n' % (i + 1) + o + b'\nendobj\n'
xref = len(pdf)
pdf += b'xref\n0 %d\n0000000000 65535 f \n' % (len(objs) + 1)
pdf += b''.join(b'%010d 00000 n \n' % o for o in offsets)
pdf += b'trailer << /Size %d /Root 1 0 R >>\nstartxref\n%d\n%%%%EOF\n' % (len(objs) + 1, xref)
put('doc', 'tiny.pdf', pdf)

# archives: tar (with a long name and a pax path), and it gzipped, xz'd and lzma'd
t = io.BytesIO()
with tarfile.open(fileobj=t, mode='w', format=tarfile.GNU_FORMAT) as tar:
    for name, data in (('a.txt', b'hello'), ('dir/' + 'long' * 40 + '.txt', b'x' * 600)):
        info = tarfile.TarInfo(name)
        info.size = len(data)
        tar.addfile(info, io.BytesIO(data))
put('archive', 'gnu.tar', t.getvalue())
p = io.BytesIO()
with tarfile.open(fileobj=p, mode='w', format=tarfile.PAX_FORMAT) as tar:
    info = tarfile.TarInfo('p/' + 'é' * 60)
    info.size = 3
    tar.addfile(info, io.BytesIO(b'abc'))
put('archive', 'pax.tar', p.getvalue())
put('archive', 'a.tar.gz', gzip.compress(t.getvalue()))
put('archive', 'a.tar.xz', lzma.compress(t.getvalue(), format=lzma.FORMAT_XZ))
put('archive', 'a.lzma', lzma.compress(b'lzma alone ' * 30, format=lzma.FORMAT_ALONE))

# zip, gzip and zlib for the kernel library
z = io.BytesIO()
with zipfile.ZipFile(z, 'w') as zf:
    zf.writestr('stored.txt', b'stored', compress_type=zipfile.ZIP_STORED)
    zf.writestr('dir/deflated.txt', b'deflated ' * 40, compress_type=zipfile.ZIP_DEFLATED)
put('zip', 'a.zip', z.getvalue())
put('zip', 'a.gz', gzip.compress(b'gzip data ' * 50))
put('zip', 'a.zlib', zlib.compress(b'zlib data ' * 50))

# a shell link: the header, a link info with a local path, then a description and arguments
flags = 0x02 | 0x04 | 0x20 | 0x80  # link info, name, arguments, unicode
lnk = struct.pack('<I16sIIIQQQIIIHHII', 0x4c, bytes.fromhex('0114020000000000c000000000000046'), flags, 0x20, 0, 0,
                  0, 0, 0, 0, 1, 0, 0, 0, 0)
path = b'C:\\Windows\\notepad.exe\0'
info = struct.pack('<IIIIII', 28 + len(path), 28, 1, 0, 28, 0) + path
lnk += info


def sdata(s):
    return struct.pack('<H', len(s)) + s.encode('utf-16-le')


lnk += sdata('Notepad') + sdata('readme.txt') + b'\0\0\0\0'
put('lnk', 'a.lnk', lnk)

# AML: the body (without the header) of the table tests/acpi.py gives QEMU
sys.path.insert(0, os.path.join(root, 'tests'))
import acpi  # noqa: E402
put('aml', 'test.aml', acpi.test_table()[36:])

# HID multi-touch: a touch screen with two fingers a report (tests/touch/check.c), then reports of 16 bytes
finger = bytes([0x09, 0x22, 0xa1, 0x02, 0x09, 0x42, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x01, 0x81, 0x02, 0x95,
                0x07, 0x81, 0x03, 0x09, 0x51, 0x25, 0x7f, 0x75, 0x08, 0x95, 0x01, 0x81, 0x02, 0x05, 0x01, 0x26, 0xff,
                0x0f, 0x75, 0x10, 0x09, 0x30, 0x81, 0x02, 0x09, 0x31, 0x81, 0x02, 0x05, 0x0d, 0xc0])
screen = (bytes([0x05, 0x0d, 0x09, 0x04, 0xa1, 0x01, 0x85, 0x05]) + finger + finger +
          bytes([0x09, 0x54, 0x25, 0x0a, 0x75, 0x08, 0x95, 0x01, 0x81, 0x02, 0xc0, 0x05, 0x0d, 0x09, 0x0e, 0xa1, 0x01,
                 0x85, 0x06, 0x09, 0x52, 0x15, 0x00, 0x25, 0x0a, 0x75, 0x08, 0x95, 0x01, 0xb1, 0x02, 0xc0]))
reports = b''
for count, xy in ((1, 2048), (3, 1800), (0, 1600), (0, 0)):
    r = bytes([5, 1, 0]) + struct.pack('<HH', xy, xy) + bytes([1, 1]) + struct.pack('<HH', xy + 100, xy)
    r += bytes([count])
    reports += r.ljust(16, b'\0')
put('hid', 'screen.bin', struct.pack('<H', len(screen)) + screen + reports)
