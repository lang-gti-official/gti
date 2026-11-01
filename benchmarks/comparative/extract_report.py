#!/usr/bin/env python3
"""Recover structured evidence from a managed remote runner's retained log."""
import argparse, base64, importlib.util, json, zlib
from pathlib import Path
p = argparse.ArgumentParser()
p.add_argument('log')
p.add_argument('output')
a = p.parse_args()
lines = Path(a.log).read_text().splitlines()
prefix = 'GTI_COMPARATIVE_REPORT_ZLIB_BASE64 '
payloads = [s[len(prefix):] for s in lines if s.startswith(prefix)]
if len(payloads) != 1:
    raise SystemExit('expected exactly one complete report payload')
r = json.loads(zlib.decompress(base64.b64decode(payloads[0], validate=True)))
if r.get('schema') != 1 or not isinstance(r.get('cases'), list):
    raise SystemExit('unknown report schema')
out = Path(a.output)
out.mkdir(parents=True, exist_ok=False)
spec = importlib.util.spec_from_file_location('runner', Path(__file__).with_name('run.py'))
m = importlib.util.module_from_spec(spec)
spec.loader.exec_module(m)
(out / 'report.json').write_text(json.dumps(r, indent=2) + '\n')
(out / 'report.md').write_text(m.markdown(r))
print(json.dumps(r['summary'], indent=2))
