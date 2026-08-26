# GTI on Gitea Actions

These workflows are the local-Gitea counterparts to `.github/workflows/`.
GitHub remains the source of truth for Git data; the Gitea repository is a
pull mirror used for additional high-capacity builds, tests, and local release
assets.

## One-time setup

1. Use Gitea 1.26 or newer (prefer the current patched release), then enable
   Actions for the mirrored repository. Gitea does not enable the
   Actions repository unit on mirrors by default.
2. Verify that the instance keeps `.gitea/workflows` before
   `.github/workflows` in `app.ini` (this is the current default):

   ```ini
   [actions]
   WORKFLOW_DIRS = .gitea/workflows,.github/workflows
   ```

   Gitea loads workflows from the first directory that exists. This ordering
   selects these Gitea-specific files while still letting repositories without
   a `.gitea/workflows` directory fall back to their GitHub workflows. Restart
   Gitea after changing `app.ini`.
3. Register a trusted repository runner whose `ubuntu-latest` label maps to
   the desired host or container, for example `ubuntu-latest:host` or
   `ubuntu-latest:docker://docker.gitea.com/runner-images:ubuntu-latest`.
   The static label is intentional: Gitea 1.26 and 1.27 do not support
   expressions in `runs-on`.
4. Optionally set `GTI_JOBS` to a positive integer in
   **Repository settings -> Actions -> Variables**. If it is unset or invalid,
   each build detects the runner's available processor count. A preinstalled
   `sccache` is used automatically, including the Gitea Actions cache backend
   when the runner exposes it.
5. Allow the workflow token to write releases in the repository's Actions
   token-permission settings. Build and test jobs remain read-only; only the
   publish jobs request release write access.

The runner environment needs Bash, Git, Python 3 with `venv` and `pip`, and
Node.js/npm. Linux jobs install CMake 3.20 or newer, a C++20 compiler, and a
supported LLVM where needed. macOS runners need a C++20 compiler and Homebrew;
the workflows install CMake and LLVM there where needed. Release builds
download GTI's pinned LLVM source and therefore also need outbound access to
GitHub release assets. An Ubuntu runner should be based on Ubuntu 24.04 so
`llvm-18-dev` is available.

## Mirror-aware triggers

- `ci.yml` runs when mirror synchronization updates `main` (and can be run
  manually).
- `nightly.yml` runs when the upstream GitHub nightly workflow moves the
  mirrored `nightly` tag. It never force-pushes into the read-only mirror.
- `release.yml` runs when a stable `vX.Y.Z` tag arrives from GitHub. It does not
  run merely because `VERSION` changed, which avoids a duplicate build before
  the upstream tag exists. Existing mirrored release notes and assets for other
  platforms are preserved; files with the same native-platform names are
  replaced by the locally built copies.

Manual nightly and stable runs consume the already-mirrored tag. They fail
clearly if that tag is missing or moves while packaging, so stale artifacts are
not published.
