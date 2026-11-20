# Pinned time-zone data

`iana.bin` contains the POSIX (no leap-second table) zones and backward aliases
from IANA tzdata **2026f**. Input:
https://data.iana.org/time-zones/releases/tzdata2026f.tar.gz
SHA-256: `01b4a91cec5167ed48c11e8ab78f2048cead78ce94b0cafcad0ae8c5a3044d3d`.

Generated using the installed `zic (tzcode) 2022g`, `-b fat`, with the files
`africa antarctica asia australasia europe northamerica southamerica etcetera backward`,
without `-L` (leap records). Regenerate the bundle using
`scripts/build_chrono_tzdb.py ZONE_DIRECTORY --version 2026f --output stdlib/std/detail/tzdb/iana.bin`.
Generation is explicit and offline; no build downloads data or executes a
third-party build script. Updating the version requires parser compatibility
and complete shipped-zone validation. The original data notice is in LICENSE.txt.

Format: ASCII `GTITZDB1`, a big-endian u32 length and ASCII release name,
a big-endian u32 entry count, then for each lexically sorted zone ID a u32
name length, name bytes, u32 TZif byte length, and TZif bytes. All lengths are
bounded on input. Each compiled application using the default database retains
this immutable version; explicit database loading supports independent updates.
