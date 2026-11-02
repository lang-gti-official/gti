#!/usr/bin/env python3
"""Manage matching GTI compiler, language server and resources (Python 3.9+)."""
import argparse
import contextlib
import fcntl
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import platform
import re
import shutil
import sys
import tarfile
import tempfile
import urllib.request

REPO = 'lang-gti-official/gti'
API = 'https://api.github.com/repos/' + REPO
DOWNLOAD = 'https://github.com/' + REPO + '/releases/download/'
STATUS = 'https://raw.githubusercontent.com/' + REPO + '/release-status/release-status.json'
TOOLS = ('gti', 'gti_lsp')
TAG = re.compile(r'v?(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)(?:-beta\.([1-9]\d*))?\Z')


def selector(value):
    if value in ('preview', 'nightly', 'beta', 'stable'):
        return value
    if not TAG.fullmatch(value):
        raise ValueError('expected preview, nightly, beta, stable, or X.Y.Z[-beta.N]')
    return 'v' + value.removeprefix('v')


def host():
    pair = (platform.system(), platform.machine().lower())
    names = {('Darwin', 'arm64'): 'darwin-arm64', ('Linux', 'x86_64'): 'linux-x64'}
    if pair not in names:
        raise ValueError('supported hosts: macOS arm64 and Linux x64')
    return names[pair]


def download(url, destination, limit=1024 * 1024 * 1024):
    request = urllib.request.Request(url, headers={'User-Agent': 'gtiup', 'Accept': 'application/vnd.github+json'})
    with urllib.request.urlopen(request, timeout=60) as response, open(destination, 'wb') as output:
        total = 0
        while True:
            chunk = response.read(1024 * 1024)
            if not chunk:
                break
            total += len(chunk)
            if total > limit:
                raise ValueError('download exceeds size limit')
            output.write(chunk)


def remote_json(url):
    with tempfile.TemporaryDirectory(prefix='gtiup-metadata-') as directory:
        p = Path(directory) / 'metadata'
        download(url, p, 8 * 1024 * 1024)
        return json.loads(p.read_text())


def release(channel):
    channel = selector(channel)
    status = None
    if channel not in ('preview', 'nightly'):
        status = remote_json(STATUS)
        if (status.get('schema') != 1 or not all(isinstance(status.get(k), list)
                                               for k in ('stable', 'yanked', 'withdrawn'))):
            raise ValueError('invalid release status; refusing installation')
        validate_status(status)
    if channel == 'stable':
        candidates = [selector(v) for v in status['stable']]
        if not candidates:
            raise ValueError('no stable release published; select nightly explicitly')
        channel = max(candidates, key=version_key)
    elif channel == 'beta':
        candidates = []
        for page in range(1, 101):
            batch = remote_json(API + f'/releases?per_page=100&page={page}')
            candidates.extend(r['tag_name'] for r in batch if not r['draft'] and r['prerelease']
                              and TAG.fullmatch(r['tag_name']) and '-beta.' in r['tag_name'])
            if len(batch) < 100:
                break
        else:
            raise ValueError('release listing exceeded pagination limit')
        if not candidates:
            raise ValueError('no beta release published; select nightly explicitly')
        channel = max(candidates, key=version_key)
    if status:
        version = channel.removeprefix('v').split('-beta.')[0]
        if any(e['version'] == version for e in status['yanked']):
            raise ValueError(f'{channel} is yanked; select another release explicitly')
        if any(e['tag'] == channel for e in status['withdrawn']):
            raise ValueError(f'{channel} is withdrawn; select another release explicitly')
    data = remote_json(API + '/releases/tags/' + channel)
    if data.get('draft') or data.get('tag_name') != channel:
        raise ValueError('release identity mismatch')
    if channel not in ('preview', 'nightly'):
        beta = '-beta.' in channel
        if bool(data.get('prerelease')) != beta:
            raise ValueError('release stage mismatch')
        if not beta and channel.removeprefix('v') not in status['stable']:
            raise ValueError('stable release is not recorded as published')
    version = channel.removeprefix('v').split('-beta.')[0]
    stem = 'gti-' + channel if channel in ('preview', 'nightly') else 'gti-v' + version
    return channel, stem, version


def validate_status(status):
    for version in status['stable']:
        if not isinstance(version, str) or not TAG.fullmatch(version) or version.startswith('v') or '-beta.' in version:
            raise ValueError('invalid stable release status')
    for field, key in (('yanked', 'version'), ('withdrawn', 'tag')):
        for entry in status[field]:
            value = entry.get(key) if isinstance(entry, dict) else None
            if not isinstance(value, str) or not TAG.fullmatch(value):
                raise ValueError('invalid release revocation status')
            if field == 'yanked' and (value.startswith('v') or '-beta.' in value):
                raise ValueError('invalid yanked version')
            if field == 'withdrawn' and (not value.startswith('v') or '-beta.' not in value):
                raise ValueError('invalid withdrawn beta')


def version_key(tag):
    m = TAG.fullmatch(tag)
    if not m:
        raise ValueError('invalid release version')
    return tuple(int(v or 0) for v in m.groups())


def extract(archive, destination):
    """Reject escapes, special files and link-parent writes before extraction."""
    with tarfile.open(archive, 'r:gz') as tar:
        members = tar.getmembers()
        paths, links = set(), set()
        total = 0
        for m in members:
            p = PurePosixPath(m.name)
            if p.is_absolute() or '..' in p.parts or not (m.isfile() or m.isdir() or m.issym()):
                raise ValueError('unsafe archive member: ' + m.name)
            if str(p) == '.':
                if not m.isdir():
                    raise ValueError('invalid archive root')
                continue
            if str(p) in paths:
                raise ValueError('duplicate archive member')
            paths.add(str(p))
            total += m.size
            if total > 4 * 1024**3:
                raise ValueError('archive exceeds extraction limit')
            if m.issym():
                target = PurePosixPath(m.linkname)
                resolved = (destination / str(p.parent) / m.linkname).resolve()
                if target.is_absolute() or '..' in target.parts or not resolved.is_relative_to(destination.resolve()):
                    raise ValueError('archive symlink escapes toolchain')
                links.add(p)
        for m in members:
            if any(parent in links for parent in PurePosixPath(m.name).parents):
                raise ValueError('archive writes through symlink')
            m.mode &= 0o777  # Never retain setuid/setgid/sticky bits.
        tar.extractall(destination, members=members)


class Manager:
    def __init__(self, home=None):
        self.home = Path(home or os.environ.get('GTIUP_HOME', '~/.gtiup')).expanduser().absolute()

    def state(self):
        p = self.home / 'settings.json'
        d = json.loads(p.read_text()) if p.exists() else {'schema': 1, 'default': None, 'toolchains': {}}
        if d.get('schema') != 1:
            raise ValueError('unsupported gtiup settings')
        return d

    def save(self, state):
        with tempfile.NamedTemporaryFile(mode='w', dir=self.home, delete=False) as f:
            json.dump(state, f, indent=2)
            f.write('\n')
            name = f.name
        os.replace(name, self.home / 'settings.json')

    @contextlib.contextmanager
    def lock(self):
        self.home.mkdir(parents=True, exist_ok=True)
        with open(self.home / '.lock', 'a') as f:
            fcntl.flock(f, fcntl.LOCK_EX)
            yield

    def selected(self):
        if os.environ.get('GTIUP_TOOLCHAIN'):
            return selector(os.environ['GTIUP_TOOLCHAIN'])
        cwd = Path.cwd()
        for folder in (cwd, *cwd.parents):
            file = folder / 'gti-toolchain'
            if file.exists():
                return selector(file.read_text().strip())
        value = self.state()['default']
        if not value:
            raise ValueError('no default toolchain; run gtiup default nightly (or beta/stable)')
        return selector(value)

    def which(self, tool, channel=None):
        if tool not in TOOLS:
            raise ValueError('tool must be gti or gti_lsp')
        channel = selector(channel) if channel else self.selected()
        entry = self.state()['toolchains'].get(channel)
        if not entry:
            raise ValueError(f'{channel} is not installed; run gtiup install {channel}')
        root = (self.home / 'toolchains' / entry['directory']).resolve()
        if not root.is_relative_to((self.home / 'toolchains').resolve()):
            raise ValueError('invalid toolchain path')
        binary = root / 'bin' / tool
        if not binary.is_file() or not os.access(binary, os.X_OK):
            raise ValueError('toolchain executable missing: ' + str(binary))
        return binary

    def install(self, channel):
        channel = selector(channel)
        with self.lock():
            tag, stem, version = release(channel)
            asset = stem + '-' + host() + '.tar.gz'
            base = DOWNLOAD + tag + '/'
            store = self.home / 'toolchains'
            store.mkdir(exist_ok=True)
            with tempfile.TemporaryDirectory(prefix='.install-', dir=store) as tmp:
                tmp = Path(tmp)
                archive, checksum = tmp / asset, tmp / 'checksum'
                download(base + asset + '.sha256', checksum, 4096)
                parts = checksum.read_text().split()
                if len(parts) != 2 or not re.fullmatch('[0-9a-f]{64}', parts[0]) or parts[1].lstrip('*') != asset:
                    raise ValueError('invalid release checksum')
                download(base + asset, archive)
                digest = sha256(archive)
                if digest != parts[0]:
                    raise ValueError('archive checksum mismatch')
                stage = tmp / 'stage'
                stage.mkdir()
                extract(archive, stage)
                for tool in TOOLS:
                    p = stage / 'bin' / tool
                    if not p.is_file() or not os.access(p, os.X_OK):
                        raise ValueError('release lacks executable ' + tool)
                packaged_version = (stage / 'share/gti/VERSION').read_text().strip()
                if not TAG.fullmatch(packaged_version) or packaged_version.startswith('v') or '-beta.' in packaged_version:
                    raise ValueError('invalid packaged VERSION')
                if tag in ('preview', 'nightly'):
                    download(base + 'BUILD_ID', tmp / 'BUILD_ID', 128)
                    identity = (tmp / 'BUILD_ID').read_text().strip()
                    if not re.fullmatch('[0-9a-f]{40}', identity) or (stage / 'share/gti/BUILD_ID').read_text().strip() != identity:
                        raise ValueError(f'{tag} changed during download; retry')
                else:
                    identity = version
                    if packaged_version != version:
                        raise ValueError('archive version mismatch')
                if tag == 'preview':
                    manifest = json.loads((stage / 'share/gti/preview.json').read_text())
                    if manifest.get('schema') != 1 or not isinstance(manifest.get('features'), list):
                        raise ValueError('preview inclusion manifest missing or invalid')
                    if (stage / 'share/gti/PREVIEW_SOURCE').read_text().strip() != identity:
                        raise ValueError('preview source mismatch')
                directory = tag + '-' + digest
                target = store / directory
                if target.is_symlink():
                    raise ValueError('toolchain directory must not be a symlink')
                if not target.exists():
                    os.rename(stage, target)
                state = self.state()
                state['toolchains'][channel] = {'directory': directory, 'tag': tag, 'identity': identity, 'sha256': digest}
                self.save(state)
                print(f'Installed {channel}: {tag} ({identity})')

    def bootstrap(self):
        with self.lock():
            folder = self.home / 'bin'
            folder.mkdir(exist_ok=True)
            destination = folder / 'gtiup'
            for name in ('gtiup', *TOOLS):
                p = folder / name
                if p.exists() or p.is_symlink():
                    if name != 'gtiup' and (not p.is_symlink() or os.readlink(p) != 'gtiup'):
                        raise ValueError('refusing to overwrite ' + str(p))
                    if name == 'gtiup' and p.resolve() != Path(__file__).resolve():
                        raise ValueError('gtiup already exists; run that installation')
            if destination.resolve() != Path(__file__).resolve():
                shutil.copyfile(__file__, destination)
                destination.chmod(0o755)
            for tool in TOOLS:
                p = folder / tool
                if not p.is_symlink():
                    p.symlink_to('gtiup')
            print(f'Add {folder} to PATH. Then run: gtiup default nightly')


def sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            h.update(block)
    return h.hexdigest()


def main(argv=None):
    manager = Manager()
    name = Path(sys.argv[0]).name
    if argv is None and name in TOOLS:
        args = sys.argv[1:]
        channel = None
        if args and args[0].startswith('+'):
            channel = selector(args.pop(0)[1:])
        binary = str(manager.which(name, channel))
        os.execv(binary, [binary, *args])
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest='command', required=True)
    sub.add_parser('self-install', help='install gtiup and proxies, without editing your shell or downloading a toolchain')
    for command in ('install', 'update', 'default', 'uninstall'):
        p = sub.add_parser(command)
        p.add_argument('channel')
    sub.add_parser('gc', help='remove unreferenced toolchains; close running GTI/LSP processes first')
    sub.add_parser('list')
    sub.add_parser('show')
    p = sub.add_parser('which'); p.add_argument('tool', choices=TOOLS)
    p = sub.add_parser('run'); p.add_argument('channel'); p.add_argument('tool', choices=TOOLS); p.add_argument('args', nargs=argparse.REMAINDER)
    args = parser.parse_args(argv)
    if args.command == 'self-install':
        manager.bootstrap()
    elif args.command in ('install', 'update'):
        manager.install(args.channel)
    elif args.command == 'default':
        channel = selector(args.channel)
        if channel not in manager.state()['toolchains']:
            manager.install(channel)
        with manager.lock():
            state = manager.state(); state['default'] = channel; manager.save(state)
        print(channel)
    elif args.command == 'uninstall':
        with manager.lock():
            state = manager.state(); channel = selector(args.channel)
            if state['default'] == channel:
                raise ValueError('select another default before uninstalling this channel')
            if channel not in state['toolchains']:
                raise ValueError('toolchain is not installed')
            # Retain immutable directories: running compiler/LSP processes may still use them.
            del state['toolchains'][channel]; manager.save(state)
    elif args.command == 'gc':
        with manager.lock():
            active = {e['directory'] for e in manager.state()['toolchains'].values()}
            store = manager.home / 'toolchains'
            if store.exists():
                for path in store.iterdir():
                    if path.name not in active and re.fullmatch(r'(?:preview|nightly|v[0-9.]+(?:-beta\.[0-9]+)?)-[0-9a-f]{64}', path.name) and path.is_dir() and not path.is_symlink():
                        shutil.rmtree(path)
                        print('Removed ' + path.name)
    elif args.command == 'list':
        for channel, entry in sorted(manager.state()['toolchains'].items()):
            print(channel, entry['tag'], entry['identity'])
    elif args.command == 'show':
        print(manager.selected()); print(manager.which('gti'))
    elif args.command == 'which':
        print(manager.which(args.tool))
    elif args.command == 'run':
        binary = manager.which(args.tool, args.channel)
        os.execv(str(binary), [str(binary), *args.args])


if __name__ == '__main__':
    try:
        main()
    except (ValueError, OSError, KeyError, TypeError, tarfile.TarError) as error:
        print('gtiup: ' + str(error), file=sys.stderr)
        sys.exit(1)
