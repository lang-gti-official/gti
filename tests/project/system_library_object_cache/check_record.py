"""Check ordered object records and forge a checksummed but malformed payload."""
from pathlib import Path
import hashlib
import sys

records = list(Path('build/gti/cache/compile-object-v2').iterdir())
assert records, 'the object cache must have been populated'
for path in records:
    record = path.read_bytes()
    key, checksum, payload = record[:64], record[64:128], record[128:]
    assert len(key) == 64 and hashlib.sha256(payload).hexdigest().encode() == checksum
    count, rest = payload.split(b'\n', 1)
    assert int(count) == 1
    length, binary = rest.split(b'\n', 1)
    assert int(length) == len(binary) and binary
    if sys.argv[1] == 'trailing':
        payload += b'unexpected trailing object data'
        path.write_bytes(key + hashlib.sha256(payload).hexdigest().encode() + payload)
