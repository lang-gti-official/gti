The Neovim plugin downloads toolchains from GitHub Releases on
`lang-gti-official/gti`. The rolling `nightly` release is rebuilt daily from
`main` for Linux x64 and Apple Silicon macOS; select it with
`vim.g.gti_channel = "nightly"`. Until stable `v<VERSION>` releases are
published on GitHub, the default stable channel stops with an error naming the
missing release and the nightly channel, and never installs another version.
