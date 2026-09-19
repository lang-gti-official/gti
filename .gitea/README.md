# GTI on Gitea Actions

The repository at
`https://git2.internal.howetotech.dev/lukeh/gti-internal` is GTI's canonical
Git host. Gitea owns branches, pull requests, CI, tags, nightly builds, stable
releases, and release assets. The workflows in `.gitea/workflows/` do not rely
on a GitHub mirror or on GitHub-created tags.

## Repository setup

1. Use Gitea 1.26 or newer and enable the Actions repository unit.
2. Keep `.gitea/workflows` in the instance's configured workflow directories.
   GTI intentionally ships no alternative-host workflow fallback.
3. Register a trusted repository runner whose `ubuntu-latest` label maps to
   the intended host or container. Gitea requires static runner labels for the
   versions used by this repository.
4. Optionally set `GTI_JOBS` to a positive integer in **Repository settings ->
   Actions -> Variables**. Otherwise each build detects the runner's available
   processors. A preinstalled `sccache` is used automatically, including the
   Actions cache backend when the runner exposes it.
5. In **Repository settings -> Actions -> General**, allow the workflow token
   a maximum of `code: write` and `releases: write`. Ordinary CI requests only
   read access; the two publication jobs explicitly request the write scopes.

The runner needs Bash, Git, Python 3 with `venv` and `pip`, and Node.js 20.9 or
newer with npm.
Linux jobs install CMake 3.20 or newer, Ninja, a C++20 compiler, and supported
LLVM packages where needed. Release builds download GTI's pinned upstream LLVM
source and therefore require outbound access to that upstream release host.
An Ubuntu runner should be based on Ubuntu 24.04 so `llvm-18-dev` is available.

The current publication jobs create one archive for the native OS and
architecture of the runner claiming `ubuntu-latest`. Before advertising
additional binary platforms from Gitea, register those native runners and add
statically labelled package jobs for them; source support for a target is not
evidence that a binary archive was published for it.

## Workflow authority

- `ci.yml` runs for every push to `main`, every pull request, and manual
  dispatch. It is the canonical remote CI signal.
- `nightly.yml` runs for every `main` commit. It packages and tests the exact
  triggering revision, refuses publication if that revision was superseded,
  then advances the mutable `nightly` tag and Gitea release. Current assets are
  uploaded before assets outside the new build are pruned.
- `release.yml` runs when `VERSION` changes on `main`, or by manual dispatch.
  It packages and tests the exact commit, rejects a conflicting existing tag,
  and lets Gitea create or update the matching `v<VERSION>` release.

Both release workflows use Gitea's repository token and publish only after the
build-tree and installed-toolchain gates pass. A successful push merely starts
that asynchronous work; local validation remains the pre-push authority.

## GitHub decommission boundary

The tracked code can operate without GitHub, but deleting the old GitHub
repository is a separate, destructive administration step. Before deletion,
make an explicit retain-or-discard decision for data that `git push` does not
copy:

- issues, pull requests, discussions, labels, milestones, and attachments;
- wiki pages and wiki history;
- release notes and historical release assets;
- repository settings, secrets, branch protection, webhooks, and deploy keys;
- every historical tag not already present in Gitea.

Bulk-pushing historical tags can execute workflow files stored in those old
tagged commits. Import tags while Actions is temporarily disabled, or use an
instance-level migration path that suppresses workflow dispatch. Verify the
Gitea clone, wiki, issue history, tags, latest stable archive, nightly archive,
and installer before deleting GitHub.

Links to third-party projects hosted on GitHub, including LLVM and vendored
upstream attribution, are external dependencies rather than GTI repository
authority and may remain.
