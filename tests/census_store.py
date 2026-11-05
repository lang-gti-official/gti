"""Per-example storage for the MIR and LoweredProgram census baselines.

Each shipped example has its own file, ``tests/census/<example>.json``:

    {
      "lowered": {"arm64-apple-macos": {...row...}, "x86_64-unknown-linux": {...}},
      "mir": {"arm64-apple-macos": 435, "x86_64-unknown-linux": 435}
    }

One corpus-wide file made two pull requests conflict whenever they changed
neighbouring rows; per-example files conflict only when both change the same
example. The census checks keep working on the documents they always used
(``examples`` for the canonical macOS profile plus sparse ``variants``):
``load_*`` assembles them from the directory and ``save_*`` writes back only
the files whose content changed. A path to a single legacy JSON document is
still accepted, which keeps the checks' unit controls simple.
"""

from __future__ import annotations

import json
from pathlib import Path

TARGETS = ("arm64-apple-macos", "x86_64-unknown-linux")


def _files(directory: Path) -> dict[str, dict]:
    result = {}
    for path in sorted(directory.glob("*.json")):
        result[path.stem + ".gti"] = json.loads(path.read_text(encoding="utf-8"))
    return result


def _document(directory: Path, key: str) -> dict:
    base, variant = {}, {}
    for name, record in _files(directory).items():
        profiles = record.get(key)
        if profiles is None:
            continue
        if not isinstance(profiles, dict) or set(profiles) != set(TARGETS):
            raise ValueError(f"{directory / (name[:-4] + '.json')}: {key} must record both {TARGETS}")
        base[name] = profiles[TARGETS[0]]
        if profiles[TARGETS[1]] != profiles[TARGETS[0]]:
            variant[name] = profiles[TARGETS[1]]
    return {"examples": base, "target": TARGETS[0],
            "variants": {TARGETS[1]: {"examples": variant}}}


def _save(directory: Path, key: str, document: dict) -> None:
    directory.mkdir(parents=True, exist_ok=True)
    base = document["examples"]
    variant = document["variants"][TARGETS[1]]["examples"]
    existing = _files(directory)
    for name in sorted(set(existing) | set(base)):
        record = dict(existing.get(name, {}))
        if name in base:
            record[key] = {TARGETS[0]: base[name], TARGETS[1]: variant.get(name, base[name])}
        else:
            record.pop(key, None)
        path = directory / (name[:-4] + ".json")
        if not record:
            path.unlink(missing_ok=True)
            continue
        text = json.dumps(dict(sorted(record.items())), indent=2) + "\n"
        if not path.exists() or path.read_text(encoding="utf-8") != text:
            path.write_text(text, encoding="utf-8")


def load(path: Path, key: str) -> dict:
    """The ``key`` census ("mir" or "lowered") as one document."""
    path = Path(path)
    if path.is_dir():
        return _document(path, key)
    return json.loads(path.read_text(encoding="utf-8"))


def save(path: Path, key: str, document: dict) -> None:
    path = Path(path)
    if path.is_dir() or not path.suffix:
        _save(path, key, document)
    else:
        path.write_text(json.dumps(document, indent=2) + "\n", encoding="utf-8")


def exists(path: Path) -> bool:
    path = Path(path)
    return path.is_dir() or path.is_file()
