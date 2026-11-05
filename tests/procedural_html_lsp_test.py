#!/usr/bin/env python3
"""Procedural interpolation queries, shared diagnostics and fresh editor snapshots."""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

from lsp_smoke_test import LspSession, lsp_position

SOURCE = '''import "demo/html.gti";
struct User { std::string_view name; User(std::string_view value) : name(value) {} };
int main() {
  User user{"Ada"};
  auto page = html!({<div><p>{user.name}</p></div>});
  return 0;
}
'''


def main():
    repository = Path(__file__).resolve().parents[1]
    with tempfile.TemporaryDirectory(prefix='gti-html-editor-') as directory:
        root = Path(directory)
        shutil.copytree(repository / 'examples/demo', root / 'demo')
        path = root / 'main.gti'
        path.write_text(SOURCE)
        uri = path.as_uri()
        session = LspSession(sys.argv[1], environment=dict(os.environ, GTI_STDLIB_PATH=str(repository / 'stdlib')))
        version = 0
        def send_source(source, wait=True):
            nonlocal version
            version += 1
            path.write_text(source)
            if version == 1:
                session.send({'jsonrpc':'2.0','method':'textDocument/didOpen','params':{'textDocument':{'uri':uri,'languageId':'gti','version':version,'text':source}}})
            else:
                session.send({'jsonrpc':'2.0','method':'textDocument/didChange','params':{'textDocument':{'uri':uri,'version':version},'contentChanges':[{'text':source}]}})
            if not wait:
                return
            response = session.receive_until(lambda m: m.get('method') == 'textDocument/publishDiagnostics' and m['params'].get('uri') == uri and m['params'].get('version') == version)
            return [d for d in response['params']['diagnostics'] if d.get('severity') == 1]
        def query(method, ident, source, marker, offset=0):
            session.send({'jsonrpc':'2.0','id':ident,'method':'textDocument/'+method,'params':{'textDocument':{'uri':uri},'position':lsp_position(source,source.index(marker)+offset)}})
            return session.receive_until(lambda m: m.get('id') == ident)
        try:
            session.send({'jsonrpc':'2.0','id':1,'method':'initialize','params':{'capabilities':{'textDocument':{'publishDiagnostics':{'relatedInformation':True}}}}})
            session.receive_until(lambda m:m.get('id') == 1)
            session.send({'jsonrpc':'2.0','method':'initialized','params':{}})
            assert not send_source(SOURCE)
            hover = query('hover',10,SOURCE,'user.name',1)
            assert hover.get('result'), hover
            definition = query('definition',11,SOURCE,'user.name',1)
            assert 'user' not in str(definition.get('error','')) and definition.get('result'), definition
            invalid = SOURCE.replace('</p>','</wrong>')
            errors = send_source(invalid)
            mismatch = next(d for d in errors if 'does not match' in d['message'])
            start = invalid.index('wrong')
            assert mismatch['range']['start'] == lsp_position(invalid,start), mismatch
            assert mismatch.get('relatedInformation'), mismatch
            cli = subprocess.run([sys.argv[2],str(path),'--backend','llvm','-O0','-o',str(root/'bad')],capture_output=True,text=True,timeout=60)
            assert cli.returncode != 0 and mismatch['message'] in cli.stderr, (mismatch,cli.stderr)
            partial = SOURCE.replace('user.name','user.')
            send_source(partial,wait=False)
            completion = query('completion',12,partial,'user.',5)
            result = completion.get('result') or {}
            items = result.get('items',[]) if isinstance(result,dict) else result
            assert any(item.get('label') == 'name' for item in items), completion
            # A later valid edit supersedes both malformed syntax and an older
            # queued bad edit. Query responses must describe the current source.
            send_source(invalid,wait=False)
            assert not send_source(SOURCE)
            session.send({'jsonrpc':'2.0','method':'textDocument/didChange','params':{'textDocument':{'uri':uri,'version':version-1},'contentChanges':[{'text':invalid}]}})
            assert query('hover',13,SOURCE,'user.name',1).get('result')
            print('HTML CLI/LSP diagnostics, embedded completion and stale edits passed')
        finally:
            session.close()


if __name__ == '__main__':
    main()
