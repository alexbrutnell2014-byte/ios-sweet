#!/usr/bin/env python3
"""Install an IPA (or .app folder) on the surya iOS port over USB.

  surya-ipa.py install APP.ipa|APP.app      re-sign ad hoc, copy, register, show it on the home screen
  surya-ipa.py list                          installed third-party apps
  surya-ipa.py uninstall BUNDLE_ID           unregister (the bundle files stay; data container too)
  surya-ipa.py resign APP.app                only re-sign a local .app in place (no phone needed)

The port's kernel accepts any ad hoc signature whose page hashes match, but a decrypted App Store binary no
longer matches its original signature, so every Mach-O in the bundle is re-signed here: a new ad hoc code
directory (SHA-256, 4 KiB pages) with the binary's own entitlements (XML + DER), written over the old
signature in place (an ad hoc signature is always smaller than an Apple-signed one, so no load commands move).
FairPlay-encrypted binaries (cryptid 1) can't run; decrypt them first.
Registration on the phone: lsreg -i (data container + System type, like TrollStore).
"""
import hashlib
import os
import plistlib
import shutil
import struct
import subprocess
import sys
import tarfile
import tempfile
import uuid
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
PAGE = 4096

MH_MAGIC_64 = 0xFEEDFACF
FAT_MAGIC = 0xCAFEBABE
FAT_MAGIC_64 = 0xCAFEBABF
LC_SEGMENT_64 = 0x19
LC_CODE_SIGNATURE = 0x1D
LC_ENCRYPTION_INFO_64 = 0x2C
MH_EXECUTE = 2
CPU_ARM64 = 0x0100000C


class SignError(Exception):
    pass


# ---- DER entitlements (Apple's format: [APPLICATION 16] { INTEGER 1, [CONTEXT 16] dict }) ----

def _der_len(n):
    if n < 0x80:
        return bytes([n])
    b = n.to_bytes((n.bit_length() + 7) // 8, 'big')
    return bytes([0x80 | len(b)]) + b


def _der(tag, body):
    return bytes([tag]) + _der_len(len(body)) + body


def _der_value(v):
    if isinstance(v, bool):
        return _der(0x01, b'\xff' if v else b'\x00')
    if isinstance(v, int):
        n = max(1, (v.bit_length() + 8) // 8)
        return _der(0x02, v.to_bytes(n, 'big', signed=True))
    if isinstance(v, str):
        return _der(0x0C, v.encode())
    if isinstance(v, list):
        return _der(0x30, b''.join(_der_value(x) for x in v))
    if isinstance(v, dict):
        items = sorted(v.items(), key=lambda kv: kv[0].encode())
        return _der(0xB0, b''.join(_der(0x30, _der(0x0C, k.encode()) + _der_value(x)) for k, x in items))
    if isinstance(v, bytes):
        return _der(0x04, v)
    raise SignError('entitlement value type %r not supported' % type(v))


def der_entitlements(ents):
    return _der(0x70, _der(0x02, b'\x01') + _der_value(ents))


# ---- Mach-O ----

def slices(data):
    magic = struct.unpack_from('>I', data, 0)[0]
    if magic in (FAT_MAGIC, FAT_MAGIC_64):
        n = struct.unpack_from('>I', data, 4)[0]
        out = []
        for i in range(n):
            if magic == FAT_MAGIC:
                cpu, sub, off, size, _ = struct.unpack_from('>iiIII', data, 8 + i * 20)
            else:
                cpu, sub, off, size, _, _ = struct.unpack_from('>iiQQII', data, 8 + i * 32)
            out.append((off, size))
        return out
    if struct.unpack_from('<I', data, 0)[0] == MH_MAGIC_64:
        return [(0, len(data))]
    return []


def is_macho(path):
    try:
        with open(path, 'rb') as f:
            h = f.read(4)
    except OSError:
        return False
    if len(h) < 4:
        return False
    return struct.unpack('<I', h)[0] == MH_MAGIC_64 or struct.unpack('>I', h)[0] in (FAT_MAGIC, FAT_MAGIC_64)


def parse_superblob(sig):
    """{type: blob bytes} from an existing embedded signature."""
    out = {}
    if len(sig) < 12 or struct.unpack_from('>I', sig, 0)[0] != 0xFADE0CC0:
        return out
    count = struct.unpack_from('>I', sig, 8)[0]
    for i in range(count):
        t, off = struct.unpack_from('>II', sig, 12 + i * 8)
        if off + 8 > len(sig):
            continue
        ln = struct.unpack_from('>I', sig, off + 4)[0]
        out[t] = sig[off:off + ln]
    return out


def sign_slice(buf, base, size, ident_default, info_hash, res_hash, log):
    """Re-sign the 64-bit Mach-O at buf[base:base+size] in place. Returns a short status."""
    magic, cpu, sub, ftype, ncmds, sizeofcmds, flags, _ = struct.unpack_from('<IiiIIIII', buf, base)
    if magic != MH_MAGIC_64:
        return 'skipped (not 64-bit)'
    p = base + 32
    sig_lc = None
    text = None
    for _ in range(ncmds):
        cmd, cmdsize = struct.unpack_from('<II', buf, p)
        if cmd == LC_CODE_SIGNATURE:
            sig_lc = p
        elif cmd == LC_ENCRYPTION_INFO_64:
            cryptid = struct.unpack_from('<I', buf, p + 16)[0]
            if cryptid != 0:
                raise SignError('FairPlay encrypted (cryptid %d): decrypt the app first' % cryptid)
        elif cmd == LC_SEGMENT_64:
            name = bytes(buf[p + 8:p + 24]).split(b'\0')[0]
            if name == b'__TEXT':
                text = struct.unpack_from('<QQ', buf, p + 40)  # fileoff, filesize
        p += cmdsize
    if sig_lc is None:
        raise SignError('no LC_CODE_SIGNATURE (unsigned binary): not supported')
    dataoff, datasize = struct.unpack_from('<II', buf, sig_lc + 8)
    old = parse_superblob(bytes(buf[base + dataoff:base + dataoff + datasize]))

    ident = ident_default
    if 0 in old:
        cd = old[0]
        io = struct.unpack_from('>I', cd, 20)[0]
        ident = cd[io:cd.index(b'\0', io)].decode()
    xml = old.get(5, b'')[8:]
    ents = plistlib.loads(xml) if xml.strip() else {}

    blobs = []  # (type, bytes), CodeDirectory built last
    req = struct.pack('>III', 0xFADE0C01, 12, 0)
    blobs.append((2, req))
    special = {2: hashlib.sha256(req).digest()}
    if info_hash:
        special[1] = info_hash
    if res_hash:
        special[3] = res_hash
    if ents:
        eb = old[5]  # the original XML, byte for byte
        blobs.append((5, eb))
        special[5] = hashlib.sha256(eb).digest()
        d = der_entitlements(ents)
        db = struct.pack('>II', 0xFADE7172, 8 + len(d)) + d
        blobs.append((7, db))
        special[7] = hashlib.sha256(db).digest()
    nspecial = max(special) if special else 0

    code_limit = dataoff
    ncode = (code_limit + PAGE - 1) // PAGE
    hashes = b''.join(hashlib.sha256(bytes(buf[base + i * PAGE:base + min((i + 1) * PAGE, code_limit)])).digest()
                      for i in range(ncode))
    ident_b = ident.encode() + b'\0'
    HDR = 88
    ident_off = HDR
    hash_off = ident_off + len(ident_b) + nspecial * 32
    spec = b''.join(special.get(i, b'\0' * 32) for i in range(nspecial, 0, -1))
    exec_base, exec_limit = text if text else (0, 0)
    exec_flags = 1 if ftype == MH_EXECUTE else 0
    cd_len = hash_off + len(hashes)
    cd = struct.pack('>IIIIIIIIIBBBBIIIIQQQQ', 0xFADE0C02, cd_len, 0x20400, 0x2, hash_off, ident_off,
                     nspecial, ncode, code_limit, 32, 2, 0, 12, 0, 0, 0, 0, 0, exec_base, exec_limit, exec_flags)
    assert len(cd) == HDR, len(cd)
    cd += ident_b + spec + hashes
    blobs.insert(0, (0, cd))
    blobs.append((0x10000, struct.pack('>II', 0xFADE0B01, 8)))  # empty CMS, as codesign -s - writes

    hdr_len = 12 + 8 * len(blobs)
    total = hdr_len + sum(len(b) for _, b in blobs)
    if total > datasize:
        raise SignError('new signature (%d bytes) does not fit the old one (%d bytes)' % (total, datasize))
    out = struct.pack('>III', 0xFADE0CC0, total, len(blobs))
    off = hdr_len
    for t, b in blobs:
        out += struct.pack('>II', t, off)
        off += len(b)
    out += b''.join(b for _, b in blobs)
    buf[base + dataoff:base + dataoff + datasize] = out + b'\0' * (datasize - len(out))
    return '%s, %d pages%s' % (ident, ncode, ', %d entitlements' % len(ents) if ents else '')


def sign_file(path, ident, info_hash=None, res_hash=None, log=print):
    with open(path, 'rb') as f:
        buf = bytearray(f.read())
    sl = slices(buf)
    notes = []
    arm64 = False
    for off, size in sl:
        cpu = struct.unpack_from('<i', buf, off + 4)[0]
        arm64 |= cpu == CPU_ARM64
        notes.append(sign_slice(buf, off, size, ident, info_hash, res_hash, log))
    with open(path, 'wb') as f:
        f.write(buf)
    return arm64, notes


def bundle_hashes(bundle):
    ih = rh = None
    ip = os.path.join(bundle, 'Info.plist')
    if os.path.exists(ip):
        ih = hashlib.sha256(open(ip, 'rb').read()).digest()
    rp = os.path.join(bundle, '_CodeSignature', 'CodeResources')
    if os.path.exists(rp):
        rh = hashlib.sha256(open(rp, 'rb').read()).digest()
    return ih, rh


def resign_app(app):
    info = plistlib.load(open(os.path.join(app, 'Info.plist'), 'rb'))
    main_exe = os.path.join(app, info['CFBundleExecutable'])
    bundles = {}
    for root, dirs, files in os.walk(app):
        for d in dirs:
            if d.endswith(('.app', '.appex', '.framework', '.xctest')):
                bundles[os.path.join(root, d)] = True
    count = 0
    main_arm64 = None
    for root, dirs, files in os.walk(app):
        for fn in files:
            path = os.path.join(root, fn)
            if os.path.islink(path) or not is_macho(path):
                continue
            # The bundle this binary is the executable of, if any: gets that bundle's Info.plist hash.
            ident = os.path.splitext(fn)[0]
            ih = rh = None
            owner = root if (root in bundles or root == app) else None
            if owner:
                bi = os.path.join(owner, 'Info.plist')
                if os.path.exists(bi):
                    bin_info = plistlib.load(open(bi, 'rb'))
                    if bin_info.get('CFBundleExecutable') == fn:
                        ident = bin_info.get('CFBundleIdentifier', ident)
                        ih, rh = bundle_hashes(owner)
            arm64, notes = sign_file(path, ident, ih, rh)
            if path == main_exe:
                main_arm64 = arm64
            count += 1
            print('  signed %s: %s' % (os.path.relpath(path, app), '; '.join(notes)))
    if main_arm64 is False:
        raise SignError('the app has no arm64 slice (arm64e-only or 32-bit): it cannot run on this phone')
    return info, count


# ---- phone ----

def files(*args):
    subprocess.run([sys.executable, os.path.join(HERE, 'surya-files.py')] + list(args), check=True,
                   stdout=subprocess.DEVNULL)


def run(*args, check=True):
    r = subprocess.run([sys.executable, os.path.join(HERE, 'surya-run.py')] + list(args), capture_output=True,
                       text=True)
    out = (r.stdout + r.stderr).strip()
    if check and r.returncode != 0:
        raise SignError('%s failed: %s' % (' '.join(args), out))
    return out


def install(src):
    tmp = tempfile.mkdtemp(prefix='surya-ipa-')
    try:
        if os.path.isdir(src):
            app = os.path.join(tmp, os.path.basename(src.rstrip('/')))
            shutil.copytree(src, app, symlinks=True)
        else:
            with zipfile.ZipFile(src) as z:
                for m in z.namelist():
                    if m.startswith('/') or '..' in m.split('/'):
                        raise SignError('bad path in IPA: %s' % m)
                z.extractall(tmp)
            pl = os.path.join(tmp, 'Payload')
            apps = [d for d in os.listdir(pl) if d.endswith('.app')] if os.path.isdir(pl) else []
            if len(apps) != 1:
                raise SignError('expected one Payload/*.app in the IPA, found %r' % apps)
            app = os.path.join(pl, apps[0])
        info, n = resign_app(app)
        bid = info['CFBundleIdentifier']
        print('%s %s (%s): %d binaries re-signed' % (info.get('CFBundleDisplayName') or info.get('CFBundleName'),
              info.get('CFBundleShortVersionString', '?'), bid, n))
        if info.get('MinimumOSVersion') and tuple(int(x) for x in info['MinimumOSVersion'].split('.')[:2]) > (16, 7):
            print('WARNING: needs iOS %s, the phone runs 16.7' % info['MinimumOSVersion'])

        tarp = os.path.join(tmp, 'app.tar')
        with tarfile.open(tarp, 'w', format=tarfile.USTAR_FORMAT) as t:
            t.add(app, arcname=os.path.basename(app))
        # Same bundle id: reuse its folder, so reinstalling updates the app in place.
        listing = run('-e', 'LSREG_UID=501', '/var/local/bin/lsreg', '-l')
        dest = None
        for line in listing.splitlines():
            parts = line.split(None, 2)
            if len(parts) == 3 and parts[0] == bid and '/Bundle/Application/' in parts[2]:
                dest = os.path.dirname(parts[2].replace('/private/var/', '/var/'))
        if dest is None:
            dest = '/var/containers/Bundle/Application/' + str(uuid.uuid4()).upper()
        try:
            files('mkdir', '/var/local/tmp', '755')
        except subprocess.CalledProcessError:
            pass
        try:
            files('rm', '/var/local/tmp/app.tar')
        except subprocess.CalledProcessError:
            pass
        # An update must not rewrite the files of a running app.
        run('/var/local/bin/psx', '-k', info['CFBundleExecutable'][:15], check=False)
        print('copying %d KB to %s' % (os.path.getsize(tarp) // 1024, dest))
        files('put', tarp, '/var/local/tmp/app.tar', '644')
        print(run('-t', '300', '/var/local/bin/untar', '/var/local/tmp/app.tar', dest))
        files('rm', '/var/local/tmp/app.tar')
        print(run('/var/local/bin/lsreg', '-i', dest + '/' + os.path.basename(app)))
        print('installed: open it from the home screen, or: tools/surya-run.py /var/local/bin/openapp ' + bid)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def main():
    if len(sys.argv) < 2 or sys.argv[1] not in ('install', 'list', 'uninstall', 'resign'):
        print(__doc__)
        return 2
    try:
        cmd = sys.argv[1]
        if cmd == 'install':
            for a in sys.argv[2:]:
                install(a)
        elif cmd == 'resign':
            for a in sys.argv[2:]:
                resign_app(a)
        elif cmd == 'list':
            for line in run('-e', 'LSREG_UID=501', '/var/local/bin/lsreg', '-l').splitlines():
                if '/Bundle/Application/' in line:
                    print(line)
        elif cmd == 'uninstall':
            listing = run('-e', 'LSREG_UID=501', '/var/local/bin/lsreg', '-l')
            for line in listing.splitlines():
                parts = line.split(None, 2)
                if len(parts) == 3 and parts[0] == sys.argv[2]:
                    print(run('/var/local/bin/lsreg', '-u', parts[2]))
                    return 0
            print('%s is not installed' % sys.argv[2])
            return 1
    except SignError as e:
        print('error: %s' % e)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
