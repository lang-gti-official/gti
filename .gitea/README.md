# GTI on Gitea

Since 2026-10-29, source, branches, pull requests, issues and CI live at
https://github.com/lang-gti-official/gti. Gitea (`git.internal.howetotech.dev`)
keeps two roles:

- the standalone reference website (`gti-lang/gti-reference`);
- the `gti add` package registry: the `gti-lang` library package repositories
  (libc and the like).

Toolchain releases (compiler archives and the Neovim plugin's downloads)
publish to GitHub Releases through `.github/workflows/nightly.yml`, and the
Neovim installer downloads from there. The existing `gti-lang/gti` release
archives stay downloadable for plugin tags cut before that switch, whose own
installer still names Gitea.

The `gti-lang/gti` source repository is frozen: its `main` merge allowlist is
empty, so no merge lands there. Its history remains readable. GitHub history
carries rewritten commit identities with identical trees; the old-to-new commit
map is retained outside the repository by the owner.

## Release publication

`workflows/release.yml` built and published the Linux x64 nightly and stable
packages when Gitea `main` advanced. Gitea `main` no longer advances, so it does
not run, and no new toolchain release is published to Gitea. It is superseded
by the GitHub nightly workflow and stays unchanged as the record of its
contract; it is never converted to publish to GitHub. Its packaging rules carry
over: one bundled build per platform with full CTest and installed-toolchain
validation, and a serialized nightly that skips superseded commits. Its stable
publication has no GitHub successor until the beta/stable release train lands.
Tokens never follow arbitrary redirects in the release synchronization helper.

## Historical automation

`ci.yml` moved to `.github/workflows/ci.yml`. Previous configurations under
`docs/plans/archive/` are historical evidence, not live policy.
