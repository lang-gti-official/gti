# GTI Highlight Inspector

This diagnostic CLI compares equivalent C++ and GTI symbols inside a real
headless Neovim session. C++ is the visual benchmark: equivalent GTI probes
should ultimately resolve to the same foreground, background, and text styles
under the `github` colorscheme.

The tool does not modify highlight definitions, queries, either LSP, the user’s
Neovim configuration, or either language implementation. Its fixtures and all
generated state live below this directory.

## Run

From the GTI repository root:

```sh
scripts/highlight-inspector/run
```

Useful options:

```sh
scripts/highlight-inspector/run --output /tmp/gti-highlight-report
scripts/highlight-inspector/run --cpp-client clangd --gti-client gti_lsp
scripts/highlight-inspector/run --fail-on-visual-difference
scripts/highlight-inspector/run --help
```

Use `--cpp-command` or `--gti-command` when the executable is not on `PATH`.
Use `--cpp-parser`, `--gti-parser`, or `--gti-runtime` to override automatic
runtime discovery. Client names and commands are intentionally separate so the
report can select semantic tokens from exactly one attached client.

## Prerequisites

- Neovim 0.11 or newer.
- `clangd` with semantic-token support.
- An installed C++ Tree-sitter parser and nvim-treesitter C++ queries.
- The GTI Neovim plugin, Tree-sitter parser, and `gti_lsp` toolchain.
- The `github_theme.nvim` plugin providing the `github` colorscheme.

The default discovery paths match a normal Lazy/LazyVim installation beneath
`stdpath("data")/lazy`. The inspector uses a dedicated init file rather than
loading the full user configuration. It adds the installed runtimes directly,
loads the real theme and GTI highlight links, and starts only the selected LSP
client for each fixture.

Neovim cache, state, logs, ShaDa, swap, backup, and undo files are disabled or
redirected to `output/`, which has its own local `.gitignore`.

## Probes and output

A probe marker names a symbol on the next non-comment source line:

```cpp
// @probe parameter.declaration.left | left
T add(const T left, const T right) {
```

The target text must occur exactly once on that line. Shared `@probe` labels
must appear in both fixtures. `@probe-cpp` and `@probe-gti` mark intentional
language-specific probes and are not treated as missing counterparts.

Successful runs write deterministic JSON:

- `output/cpp.json`: raw C++ Tree-sitter, selected clangd token, highlight
  groups, links, priorities, and resolved visual attributes.
- `output/gti.json`: the corresponding GTI data from `gti_lsp`.
- `output/comparison.json`: records joined by probe label, with Tree-sitter,
  semantic type, semantic modifier, and final visual comparisons kept
  independent.

Raw reports retain every capture returned by Neovim. The joined comparison
deduplicates identical capture/priority pairs so inherited C and C++ query
layers do not create false structural differences solely through repetition.

The terminal prints one status row per shared probe. Semantic differences such
as GTI’s default `readonly` modifier remain visible and can be marked
intentional by the small allowlist in `compare.lua`; they are never hidden.
Final visual differences are always reported because C++ is the benchmark.

Process-local LSP client IDs are recorded for debugging but are not used when
joining or comparing reports. The inspector starts clients in a fixed order,
so the IDs are stable in normal consecutive runs; consumers should still treat
them as runtime metadata.

By default, a completed inspection exits zero even when differences are found.
`--fail-on-visual-difference` exits nonzero only for final visual mismatches.
Missing prerequisites, malformed or duplicate probes, parser errors, LSP or
semantic-token timeouts, missing shared probes, and output failures always exit
nonzero with an actionable message.

## Known limitation

Neovim exposes each Tree-sitter and semantic-token highlight layer rather than
a rendered terminal cell. The inspector resolves every group through
`nvim_get_hl()` and composes applicable attributes in priority order, beginning
with `Normal`. This models the normal Tree-sitter/semantic-token stack and keeps
the source of a mismatch auditable, but UI-only decorations from unrelated
plugins are deliberately absent from the dedicated headless session.
