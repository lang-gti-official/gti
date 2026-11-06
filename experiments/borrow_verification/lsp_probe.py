#!/usr/bin/env python3
"""Opt-in CLI/probe/LSP diagnostic and changed-snapshot comparisons."""
import argparse
import json
import os
from pathlib import Path
import re
import sys
import time

from corpus import cases
from runner import ROOT, analyze, command, provenance, text

sys.path.insert(0, str(ROOT / 'tests'))
from lsp_smoke_test import LspSession, lsp_position

SELECTED = ('shared_write_conflict', 'parent_while_child_live', 'returned_local',
            'temporary_mutable', 'move_while_borrowed', 'branch_move_join',
            'user_view_local_escape')
GOOD = 'int main() { mut int x = 1; int& a = x; return a - 1; }\n'
PREFIX = '// UTF-16 position check: 😀\n'


def position(source, byte_offset):
    prefix = source.encode('utf-8')[:byte_offset].decode('utf-8')
    return lsp_position(source, len(prefix))


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--build', type=lambda x: Path(x).resolve(), required=True)
    p.add_argument('--probe', type=lambda x: Path(x).resolve(), required=True)
    p.add_argument('--out', type=lambda x: Path(x).resolve(), required=True)
    p.add_argument('--timeout', type=float, default=30)
    args = p.parse_args()
    if args.timeout <= 0 or args.timeout > 60:
        p.error('timeout must be between 0 and 60 seconds')
    if (args.out / 'report.json').exists():
        p.error('Choose a fresh evidence directory')
    args.out.mkdir(parents=True, exist_ok=True)
    report = {'identity': provenance(args), 'cases': [], 'passed': False}
    started = time.perf_counter()
    session = LspSession(str(args.build / 'gti_lsp'), environment={
        **os.environ, 'GTI_STDLIB_PATH': str(ROOT / 'stdlib')})
    uri = (args.out / 'open-document.gti').as_uri()
    version = 0

    def publish(source):
        nonlocal version
        version += 1
        document = {'uri': uri, 'version': version}
        if version == 1:
            document.update(languageId='gti', text=source)
            method, params = 'textDocument/didOpen', {'textDocument': document}
        else:
            method, params = 'textDocument/didChange', {
                'textDocument': document, 'contentChanges': [{'text': source}]}
        session.send({'jsonrpc': '2.0', 'method': method, 'params': params})
        reply = session.receive_until(lambda m:
            m.get('method') == 'textDocument/publishDiagnostics'
            and m['params'].get('uri') == uri
            and m['params'].get('version') == version, timeout=args.timeout)
        return [d for d in reply['params']['diagnostics'] if d.get('severity') == 1]

    try:
        session.send({'jsonrpc': '2.0', 'id': 1, 'method': 'initialize', 'params': {
            'capabilities': {'textDocument': {'publishDiagnostics': {'relatedInformation': True}}}}})
        reply = session.receive_until(lambda m: m.get('id') == 1, timeout=args.timeout)
        assert 'error' not in reply, reply
        session.send({'jsonrpc': '2.0', 'method': 'initialized', 'params': {}})
        assert not publish(PREFIX + GOOD)
        by_name = {c.name: c for c in cases()}
        for name in SELECTED:
            case = by_name[name]
            source = PREFIX + case.source
            directory = args.out / name
            record = analyze(source, args, directory)
            report['cases'].append(record)
            record['name'] = name
            assert record['classification'] == 'rejected', record
            diagnostics = publish(source)
            record['lsp_diagnostics'] = diagnostics
            expected = [d for d in record['probe']['diagnostics'] if d['severity'] == 1]
            assert {d['code'] for d in expected} == {d['code'] for d in diagnostics}
            for diagnostic in expected:
                span = {'start': position(source, diagnostic['start']),
                        'end': position(source, diagnostic['end'])}
                assert any(d['code'] == diagnostic['code'] and d['range'] == span
                           for d in diagnostics), (diagnostic, diagnostics)
            cli = command([args.build / 'gti', directory / 'program.gti', '--emit-mir',
                           '-o', directory / 'program.mir'], directory, 'cli', args.timeout)
            record['cli'] = cli
            assert cli['status'] == 'completed' and cli['exit_code'] != 0, cli
            codes = set(re.findall(r'GTI-[A-Z][0-9]+', text(cli['stderr']) + text(cli['stdout'])))
            assert {d['code'] for d in expected}.issubset(codes), codes
            assert not any(code.startswith('GTI-B000') for code in codes), codes
            record['recovery_diagnostics'] = publish(PREFIX + GOOD)
            assert not record['recovery_diagnostics'], record
            record['passed'] = True
        report['passed'] = True
    except Exception as error:
        report['error'] = str(error)
    finally:
        try:
            session.close()
        except Exception as error:
            report['passed'] = False
            report['close_error'] = str(error)
            session.process.kill()
            session.process.wait()
        report['seconds'] = time.perf_counter() - started
        report['document_versions'] = version
        (args.out / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({'passed': report['passed'], 'cases': len(report['cases']),
                      'seconds': report['seconds'], 'report': str(args.out / 'report.json')}))
    return 0 if report['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
