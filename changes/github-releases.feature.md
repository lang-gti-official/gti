The Neovim plugin downloads toolchains from GitHub Releases on
`lang-gti-official/gti`. The rolling `nightly` release is rebuilt daily from
`main` for Linux x64 and Apple Silicon macOS; select it with
`vim.g.gti_channel = "nightly"`. When the plugin's version has no stable
`v<VERSION>` release yet, the default stable channel stops with an error naming
the missing release and the nightly channel, and never installs another
version.
