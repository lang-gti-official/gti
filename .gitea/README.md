# GTI on Gitea

Since 2026-10-29, source, branches, pull requests, issues and CI live at
https://github.com/lang-gti-official/gti. Gitea (`git.internal.howetotech.dev`)
keeps two roles:

- the standalone reference website (`gti-lang/gti-reference`);
- the `gti add` package registry: the `gti-lang` library package repositories
  (libc and the like).

Toolchain releases (compiler archives and the Neovim plugin's downloads) move
to GitHub Releases. The existing `gti-lang/gti` release archives stay
downloadable, and the installer keeps using them until the GitHub release
workflow and installer switch land.

The `gti-lang/gti` source repository is frozen: its `main` merge allowlist is
empty, so no merge lands there. Its history remains readable. GitHub history
carries rewritten commit identities with identical trees; the old-to-new commit
map is retained outside the repository by the owner.

## Release publication

`workflows/release.yml` built and published the Linux x64 nightly and stable
packages when Gitea `main` advanced. Gitea `main` no longer advances, so it does
not run, and no new toolchain release is published to Gitea. Its packaging
rules carry over to the GitHub release workflow that replaces it: one bundled
build per platform with full CTest and installed-toolchain validation, stable
only on a deliberate release cut, immutable stable tags, and serialized nightly
that skips superseded commits.
Tokens never follow arbitrary redirects in the release synchronization helper.

## Historical automation

`ci.yml` moved to `.github/workflows/ci.yml`. Previous configurations under
`docs/plans/archive/` are historical evidence, not live policy.
