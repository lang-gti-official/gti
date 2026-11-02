#!/usr/bin/env python3
"""Publish packaged preview admission results, independently of nightly/full CI."""
import argparse
import base64
import json
import os
import re
from pathlib import Path
import subprocess
import tarfile

from release_artifacts import PLATFORMS, verify
from resolve_release_source import github_request

REPOSITORY = 'lang-gti-official/gti'
BASE = '/repos/' + REPOSITORY
REMOTE = 'https://github.com/' + REPOSITORY + '.git'


def checked_sha(value):
    if not re.fullmatch(r"[0-9a-f]{40}", value):
        raise ValueError("invalid source commit")
    return value


def manifest(document):
    if document.get('schema') != 1 or not isinstance(document.get('features'), list):
        raise ValueError('preview inclusion manifest missing or invalid')
    return document


def select(request, event, ref):
    if (event, ref) not in (('push', 'refs/heads/preview'), ('workflow_dispatch', 'refs/heads/main')):
        raise ValueError('preview publication requires preview push or main dispatch')
    sha = checked_sha(request(BASE + '/git/ref/heads/preview')['object']['sha'])
    content = request(BASE + '/contents/.gti-preview.json?ref=' + sha)
    manifest(json.loads(base64.b64decode(content['content'], validate=False)))
    return sha


def command(*args):
    return subprocess.run(args, check=True, capture_output=True, text=True).stdout.strip()


def stage(source, destination, sha):
    sha = checked_sha(sha)
    if command('git', '-C', str(source), 'rev-parse', 'HEAD') != sha:
        raise ValueError('preview source identity changed')
    if command('git', '-C', str(source), 'status', '--porcelain', '--untracked-files=no'):
        raise ValueError('preview source changed during validation')
    data = manifest(json.loads((source / '.gti-preview.json').read_text()))
    target = destination / 'share/gti'
    (target / 'preview.json').write_text(json.dumps(data, sort_keys=True) + '\n')
    (target / 'PREVIEW_SOURCE').write_text(sha + '\n')


def check_packages(directory, sha, run_id, attempt):
    verify(directory, sha, run_id, attempt, REPOSITORY, channel='preview')
    inclusion = None
    for platform in PLATFORMS:
        with tarfile.open(directory / f'gti-preview-{platform}.tar.gz', 'r:gz') as archive:
            def read(name):
                members = [m for m in archive if m.name.removeprefix('./') == 'share/gti/' + name]
                if len(members) != 1 or not members[0].isfile() or members[0].size > 8 * 1024 * 1024:
                    raise ValueError('invalid preview archive metadata')
                return archive.extractfile(members[0]).read().decode()
            if read('BUILD_ID').strip() != sha or read('PREVIEW_SOURCE').strip() != sha:
                raise ValueError('packaged preview has wrong source')
            data = manifest(json.loads(read('preview.json')))
            if inclusion is not None and data != inclusion:
                raise ValueError('preview platforms have different inclusion manifests')
            inclusion = data
    return inclusion


def admit(request, sha):
    if checked_sha(request(BASE + '/git/ref/heads/preview')['object']['sha']) != checked_sha(sha):
        raise ValueError('preview advanced; refusing stale publication')


def publish(directory, sha, request, run_id, attempt):
    # Recheck everything immediately before moving the dedicated rolling tag.
    inclusion = check_packages(directory, sha, run_id, attempt)
    admit(request, sha)
    refs = command('git', 'ls-remote', REMOTE, 'refs/tags/preview').split()
    lease = checked_sha(refs[0]) if refs else ''
    command('git', 'fetch', '--no-tags', REMOTE, sha)
    # A new preview push cannot silently overwrite a concurrent publication.
    admit(request, sha)
    command('git', 'push', '--porcelain', '--force-with-lease=refs/tags/preview:' + lease,
            'origin', sha + ':refs/tags/preview')
    notes = directory.parent / 'preview-notes.txt'
    notes.write_text(f'Combined preview source `{sha}`.\n\n'
                     'Includes work whose main review/full integration may still be pending.\n'
                     'Both packaged platforms passed the registered combined preview admission and installed checks.\n'
                     'This is not nightly, beta or stable validation. See preview.json for inclusion records.\n')
    exists = subprocess.run(['gh', 'release', 'view', 'preview', '--repo', REPOSITORY],
                            capture_output=True).returncode == 0
    action = 'edit' if exists else 'create'
    argv = ['gh', 'release', action, 'preview', '--repo', REPOSITORY, '--prerelease', '--latest=false',
            '--title', 'GTI preview', '--notes-file', str(notes)]
    if not exists:
        argv.append('--verify-tag')
    command(*argv)
    (directory / 'preview.json').write_text(json.dumps(inclusion, sort_keys=True) + '\n')
    (directory / 'BUILD_ID').write_text(sha + '\n')
    files = sorted(p for p in directory.iterdir() if p.name != 'BUILD_ID')
    command('gh', 'release', 'upload', 'preview', '--repo', REPOSITORY, '--clobber', *map(str, files))
    # Commit marker last. Installers racing uploads fail identity/checksum checks.
    command('gh', 'release', 'upload', 'preview', '--repo', REPOSITORY, '--clobber', str(directory / 'BUILD_ID'))


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('mode', choices=('select', 'stage', 'check', 'publish'))
    p.add_argument('--source', type=Path)
    p.add_argument('--directory', type=Path)
    p.add_argument('--sha')
    args = p.parse_args()
    if os.environ['GITHUB_REPOSITORY'] != REPOSITORY:
        raise ValueError('preview publication is restricted to the canonical repository')
    if args.mode == 'stage':
        stage(args.source, args.directory, args.sha)
    elif args.mode == 'check':
        check_packages(args.directory, checked_sha(args.sha), int(os.environ['GITHUB_RUN_ID']),
                       int(os.environ['GITHUB_RUN_ATTEMPT']))
    else:
        request = github_request(os.environ['GH_TOKEN'])
        if args.mode == 'select':
            sha = select(request, os.environ['GITHUB_EVENT_NAME'], os.environ['GITHUB_REF'])
            with open(os.environ['GITHUB_OUTPUT'], 'a') as output:
                output.write('sha=' + sha + '\n')
        else:
            publish(args.directory, checked_sha(args.sha), request, int(os.environ['GITHUB_RUN_ID']),
                    int(os.environ['GITHUB_RUN_ATTEMPT']))


if __name__ == '__main__':
    main()
