# GTI on Gitea

Since 2026-10-29, source, branches, pull requests, issues and CI live at
https://github.com/lang-gti-official/gti. Gitea (`git.internal.howetotech.dev`)
keeps two roles until GitHub is proven for them:

- the standalone reference website (`gti-lang/gti-reference`);
- published packages: toolchain release assets in `gti-lang/gti` releases,
  which the Lazy installer downloads, and the `gti-lang` package repositories
  and registry.

The `gti-lang/gti` source repository is frozen: its `main` merge allowlist is
empty, so no merge lands there. Its history remains readable. GitHub history
carries rewritten commit identities with identical trees; the old-to-new commit
map is retained outside the repository by the owner.

## Release publication

`workflows/release.yml` built and published the Linux x64 nightly and stable
packages when Gitea `main` advanced. Gitea `main` no longer advances, so it does
not run. Until a GitHub workflow publishes packages to Gitea releases,
publication is a manual owner step. Its packaging rules still apply: one
bundled Linux build with full CTest and installed-toolchain validation, stable
only on a VERSION change or an exact-source dispatch, immutable stable tags, and
serialized nightly that skips superseded commits.

Apple Silicon stable releases are manually built, validated, packaged and
uploaded to the matching Gitea release. Follow the
[manual macOS contract](../docs/architecture/build-and-driver.md#manual-macos-release-publication).
Tokens never follow arbitrary redirects in the release synchronization helper.

## Historical automation

`ci.yml` moved to `.github/workflows/ci.yml`. Previous configurations under
`docs/plans/archive/` are historical evidence, not live policy.
