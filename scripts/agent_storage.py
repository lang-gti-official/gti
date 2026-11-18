"""Select new agent storage; never redirect an existing worktree or run."""
from __future__ import annotations

import os
from pathlib import Path
import platform


class StorageError(RuntimeError):
    pass


def external_projects() -> Path | None:
    """Only an existing, writable external mount qualifies; no mkdir on probes."""
    host = platform.node().lower().replace('-', '').replace('_', '').replace(' ', '')
    if platform.system() != 'Darwin' or 'macmini' not in host:
        return None
    try:
        target = (Path.home() / 'Documents/External-projects').resolve(strict=True)
        volume = volume_for(target)
        if (volume is None or not volume.is_mount() or not target.is_dir()
                or volume.stat().st_dev == Path.home().stat().st_dev
                or not os.access(target, os.W_OK | os.X_OK)):
            return None
        return target
    except (OSError, RuntimeError):
        return None


def volume_for(path: Path) -> Path | None:
    parts = path.parts
    return Path(*parts[:3]) if len(parts) >= 3 and parts[:2] == ('/', 'Volumes') else None


def require_available(path: Path) -> None:
    """Pinned external destinations fail closed if their volume disappears."""
    volume = volume_for(path)
    if volume is not None and not volume.is_mount():
        raise StorageError(f'External volume is unavailable: {volume}; reconnect it before resuming {path}')


def worktree_path(task: str, primary: Path) -> Path:
    external = external_projects()
    if external is not None:
        return external / 'gti/worktrees' / task
    return primary.parent / 'gti-worktrees' / task


def runner_root(fallback: str) -> str:
    external = external_projects()
    return str(external / 'gti/gti-remote-runner') if external is not None else fallback


def scratch_directory() -> Path | None:
    external = external_projects()
    if external is None:
        return None
    path = external / 'gti/scratch'
    require_available(path)
    try:
        path.mkdir(parents=True, exist_ok=True)
    except PermissionError:
        return None
    return path
