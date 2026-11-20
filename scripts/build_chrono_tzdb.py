#!/usr/bin/env python3
"""Pack an explicitly compiled POSIX TZif directory; never downloads or runs code."""
import argparse
import hashlib
import json
import struct
from pathlib import Path


def pack(root: Path, version: str) -> bytes:
    files = sorted(p for p in root.rglob('*') if p.is_file())
    entries = []
    for path in files:
        name = path.relative_to(root).as_posix().encode('ascii')
        data = path.read_bytes()
        if not data.startswith(b'TZif'):
            continue
        if len(data) > 4 * 1024 * 1024:
            raise ValueError(f'oversized zone: {path}')
        entries.append((name, data))
    if not entries:
        raise ValueError('no TZif entries')
    def field(data):
        return struct.pack('>I', len(data)) + data
    result = b'GTITZDB1' + field(version.encode('ascii')) + struct.pack('>I', len(entries))
    for name, data in entries:
        result += field(name) + field(data)
    if len(result) > 64 * 1024 * 1024:
        raise ValueError('oversized database')
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path)
    parser.add_argument('--version', required=True)
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    data = pack(args.directory, args.version)
    args.output.write_bytes(data)
    print(json.dumps({'bytes': len(data), 'sha256': hashlib.sha256(data).hexdigest()}))


if __name__ == '__main__':
    main()
