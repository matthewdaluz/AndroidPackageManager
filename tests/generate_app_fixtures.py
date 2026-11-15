"""Generate tiny APK-shaped fixtures; no executable code or signing keys."""
import hashlib
import json
from pathlib import Path
import struct
import sys
import zipfile

root = Path(sys.argv[1])
root.mkdir(parents=True, exist_ok=True)
p16 = lambda n: struct.pack('<H', n)
p32 = lambda n: struct.pack('<I', n)
NONE = 0xffffffff


def axml(package, permissions, utf16=False):
    strings = ['manifest', 'package', package, 'uses-permission', 'name',
               'http://schemas.android.com/apk/res/android', *permissions]
    payload = bytearray()
    offsets = []
    for value in strings:
        offsets.append(len(payload))
        if utf16:
            data = value.encode('utf-16-le')
            payload += p16(len(data) // 2) + data + b'\0\0'
        else:
            data = value.encode()
            assert len(data) < 128
            payload += bytes([len(value), len(data)]) + data + b'\0'
    while len(payload) % 4:
        payload.append(0)
    start = 28 + len(strings) * 4
    pool = p16(1) + p16(28) + p32(start + len(payload))
    pool += p32(len(strings)) + p32(0) + p32(0 if utf16 else 0x100)
    pool += p32(start) + p32(0) + b''.join(p32(n) for n in offsets) + payload

    def element(name, attribute, value, namespace=NONE):
        node = p16(0x102) + p16(16) + p32(56) + p32(1) + p32(NONE)
        node += p32(NONE) + p32(name) + p16(20) + p16(20) + p16(1) + p16(0) * 3
        node += p32(namespace) + p32(attribute) + p32(value) + p16(8) + bytes([0, 3]) + p32(value)
        return node

    body = pool + element(0, 1, 2)
    for i in range(len(permissions)):
        body += element(3, 4, 6 + i, 5)
    return p16(3) + p16(8) + p32(8 + len(body)) + body


for name, package, perms, utf16, compression in [
    ('com.google.android.gms-1.apk', 'com.google.android.gms',
     ['android.permission.INTERNET', 'android.permission.FAKE_PACKAGE_SIGNATURE'], False, zipfile.ZIP_DEFLATED),
    ('com.android.vending-1.apk', 'com.android.vending',
     ['android.permission.INTERNET', 'android.permission.GET_ACCOUNTS'], True, zipfile.ZIP_STORED),
]:
    with zipfile.ZipFile(root / name, 'w', compression=compression) as out:
        out.writestr('AndroidManifest.xml', axml(package, perms, utf16))
        out.writestr('classes.dex', b'fixture only')
with zipfile.ZipFile(root / 'no-manifest.apk', 'w') as out:
    out.writestr('other-file', 'not a manifest')
(root / 'invalid.apk').write_bytes(b'not an APK')
for name, body in [
    ('bad-xml.apk', axml('org.example.bad', [])[:20]),
    ('oversized-manifest.apk', b'x' * (4 * 1024 * 1024 + 1)),
]:
    with zipfile.ZipFile(root / name, 'w', compression=zipfile.ZIP_DEFLATED) as out:
        out.writestr('AndroidManifest.xml', body)
malformed = bytearray(axml('org.example.bad', []))
struct.pack_into('<I', malformed, 16, 0xffffffff)
with zipfile.ZipFile(root / 'bad-string-pool.apk', 'w') as out:
    out.writestr('AndroidManifest.xml', malformed)


assets = []
for path in sorted(root.glob('com.*.apk')):
    assets.append(dict(name=path.name,
                       browser_download_url='https://github.com/microg/GmsCore/releases/download/v1/' + path.name,
                       size=path.stat().st_size,
                       digest='sha256:' + hashlib.sha256(path.read_bytes()).hexdigest()))
assets.append(dict(assets[1], name='com.google.android.gms-1-hw.apk'))
assets.append(dict(assets[1], name='com.google.android.gms-1.apk.asc'))
release = dict(tag_name='v1', published_at='2026-01-01T00:00:00Z', draft=False,
               prerelease=False, assets=assets)
(root / 'releases.json').write_text(json.dumps([release]))
variants = {
    'draft': [dict(release, draft=True)],
    'prerelease': [dict(release, prerelease=True)],
    'incomplete': [dict(release, tag_name='v2', published_at='2026-02-01T00:00:00Z', assets=[]), release],
    'ambiguous': [dict(release, assets=assets + [dict(assets[0], name='com.android.vending-2.apk')])],
    'unsigned': [dict(release, assets=[dict(a, digest=None) for a in assets])],
    'wrong-host': [dict(release, assets=[dict(a, browser_download_url='http://evil.example/app.apk') for a in assets])],
}
for name, data in variants.items():
    (root / (name + '.json')).write_text(json.dumps(data))
