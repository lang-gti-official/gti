"""Create a fixture whose lexical evidence and serialized return literal disagree."""
from hashlib import sha256
from pathlib import Path

artifact = Path("rack.gtilib")
data = artifact.read_bytes()
payload, checksum = data[:-64], data[-64:]
assert sha256(payload).hexdigest().encode() == checksum
recipe, separator, witness = payload.rpartition(b"314159")
assert separator and b"314159" in recipe
payload = recipe + b"314158" + witness
artifact.write_bytes(payload + sha256(payload).hexdigest().encode())
