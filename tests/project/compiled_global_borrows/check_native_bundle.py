"""Check the native-only envelope and prepare a mismatched metadata/object pair."""
from hashlib import sha256
from pathlib import Path
from struct import unpack_from

bundle = Path("meter.gtilib").read_bytes()
payload = bundle[:-64]
assert sha256(payload).hexdigest().encode() == bundle[-64:]
assert payload[:8] == b"GTILIB\r\n"
# Version 2, native envelope 1: an object and declarations, no optional MIR.
assert unpack_from("<II", payload, 8) == (2, 1)
# Swap the certification of two overloads, keeping the same framing and size.
# Recompute the container digest so the rejection must come from the native ABI.
safe = payload.index(b"4:none\n")
unknown = payload.index(b"7:unknown\n")
parts = [(safe, b"4:none", b"7:unknown"), (unknown, b"7:unknown", b"4:none")]
for start, old, new in sorted(parts, reverse=True):
    payload = payload[:start] + new + payload[start + len(old):]
Path("mismatched.gtilib").write_bytes(payload + sha256(payload).hexdigest().encode())
