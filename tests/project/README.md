# tests/project

Driver, `gti.toml`, packaging and CLI behaviour. A directory holding a
`test.transcript` is one test; everything else in that directory (and below
it) is the project the commands run against. `gti-test` finds it by walking,
so adding a test edits no registration.

```
tests/project/dependency_warnings/
  test.transcript
  app/gti.toml, app/src/main.gti
  warning-library/…, warning-leaf/…
```

The directory is copied to a temporary location (the transcript itself is
left out) and the commands run there in order, with `HOME`,
`XDG_CACHE_HOME` and `XDG_CONFIG_HOME` inside the temporary directory,
`NO_COLOR=1`, `TERM=dumb` and stdin from `/dev/null`. A failing command stops
the transcript.

```
// The first line says what the transcript checks; later `//` lines are
// comments.
$ cd app
$ gti check
stderr: $DIR/app/src/main.gti:2:14: warning[GTI-S2096]
stdout: Diagnostics: 0 errors, 1 warnings
$ gti build --verbose
$ gti build --verbose
stderr: gti: cache hit
$ gti check --profile nosuch
? 65
!stdout: Checked
```

| Line | Meaning |
| --- | --- |
| `$ program args…` | run a command; `gti` and `gti_lsp` are the built tools, other programs come from `PATH`; `"…"` groups words, `$DIR` is the copied directory |
| `$ cd dir` | change the directory later commands run in |
| `? N` | the exit code (default 0) |
| `stdout: text`, `stderr: text` | that stream contains `text` |
| `!stdout: text`, `!stderr: text` | that stream does not contain `text` |

Output is normalised before matching: the copied directory becomes `$DIR`,
the standard library root `$STD`, the `gti` path `gti`, and durations such
as `0.63s` or `12 ms` become `<time>`. All commands together get 20 s of CPU
(scaled by `GTI_TEST_TIME_SCALE`); a test that needs more belongs in
tests/slow. There is nothing to bless: expectations are written by hand.
