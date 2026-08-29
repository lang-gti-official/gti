# Resource and crash reproducers

These inputs are **not** part of the `run_coverage.sh` sweep. Each one makes
the compiler crash, hang, or consume unbounded memory, so running them
unattended is a bad idea. They are kept here because each is a compiler defect
with a stable trigger, recorded in `../FINDINGS.md` as F18–F20.

Run them one at a time, under a hard cap. `ulimit -v` is not honoured on
macOS, so cap resident size externally — for example with a watchdog that
polls `ps` and kills the process group.

| Input | Expect | Finding |
| --- | --- | --- |
| `operator-chain-500.gti` | SIGSEGV (exit 139), stack exhausted | F18 |
| `namespace-depth-63.gti` | unbounded memory, no diagnostic, no termination | F19 |
| `namespace-depth-62-control.gti` | `GTI-P0005` in ~0.3s, exit 65 | F19 control |
| `formatter-blank-lines.gti` | `format` is not idempotent (LSP path) | F20 |
| `formatter-token-resplit.gti` | formatting changes the token stream | F20 |

`namespace-depth-62-control.gti` is the pair to `namespace-depth-63.gti`: the
two differ by one level of nesting and by everything else about what the
compiler does with them.
