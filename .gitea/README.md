# GTI on Gitea Actions

The repository at
`https://git.internal.howetotech.dev/gti-lang/gti` is GTI's canonical
Git host. Gitea owns branches, pull requests, CI, tags, nightly builds, stable
releases, and release assets. The workflows in `.gitea/workflows/` do not rely
on a GitHub mirror or on GitHub-created tags.

## Repository setup

1. Use Gitea 1.26 or newer and enable the Actions repository unit.
2. Keep `.gitea/workflows` in the instance's configured workflow directories.
   GTI intentionally ships no alternative-host workflow fallback.
3. Register a trusted repository runner whose `ubuntu-24.04` label maps to
   the intended host or container. Gitea requires static runner labels for the
   versions used by this repository.
4. Optionally set `GTI_JOBS` and `GTI_TEST_JOBS` to positive integers in
   **Repository settings -> Actions -> Variables**. Defaults are two compile
   jobs and one CTest worker. A container's visible CPU count is not its resource
   budget; the workflows deliberately do not expand to every host CPU.
   A preinstalled `sccache` is used automatically, including the
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
architecture of the runner claiming `ubuntu-24.04`. Before advertising
additional binary platforms from Gitea, register those native runners and add
statically labelled package jobs for them; source support for a target is not
evidence that a binary archive was published for it.

For one shared runner host, start with `runner.capacity: 1` and
`runner.timeout: 6h` in the runner configuration. Four concurrent jobs can mean
four independent LLVM/compiler builds, not four compiler processes. The former
three-hour limit cancelled the stable and sanitizer jobs during execution.
Tune capacity only after measuring the host's memory/CPU budget. Restart the
runner after changing its configuration; repository variables cannot alter its
job capacity or timeout.

The native and release jobs execute a small, race-free C++ sanitizer probe
before their expensive builds. `ThreadSanitizer: unexpected memory mapping`
from this probe is a host/toolchain address-space failure, not a GTI race.
Check the compiler's sanitizer runtime against the host kernel, address-space
limits, and container seccomp policy. Modern LLVM may need the `personality`
system call when re-executing with a compatible mapping. Use a suitably
configured dedicated runner/VM and retest the probe in the exact job container;
do not skip sanitizers or enable privileged containers as a blanket fix.
The workflows do not change host ASLR or sandbox policy automatically.

After a hostname migration, also check Gitea's configured server `ROOT_URL`,
runner registration URL, and reverse-proxy routing. The canonical URL is now
`https://git.internal.howetotech.dev/`; old tagged plugin revisions retain their
old installer URL. A Git clone alone does not migrate historical release assets.
The plugin's stable channel requires a successfully published matching version
and native-platform archive, while nightly requires its archive and `BUILD_ID`.
With only a Linux runner there is no macOS archive: macOS users need a locally
built toolchain or an added native macOS package job.

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
