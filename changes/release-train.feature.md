Stable `v<VERSION>` toolchain releases are published again, now on GitHub
Releases. Every 48 hours the release train cuts the published nightly into a
`v<VERSION>-beta.<K>` prerelease for Linux x64 and Apple Silicon macOS, and
promotes a beta that has run for 48 hours without a reported release blocker
to the stable `v<VERSION>` release with the identical archives. Lazy's
`version = "*"` picks up only stable tags. The stable channel of the Neovim
installer now refuses a version that has been yanked, names the reason and the
newest stable release that is not yanked, and never installs another version
in its place.
