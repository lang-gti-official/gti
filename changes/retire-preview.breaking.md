Retire the `preview` toolchain channel. Nothing publishes it any more, so
`gtiup install preview` stops receiving updates; switch to `nightly`, which
publishes the newest commit that passed full validation. The local macOS
installer (`scripts/install_neovim_local.py`) now builds the published nightly
commit by default, `--local` still builds the current checkout, and `--preview`
fails with an explanation instead of building another source. To use your own
build through gtiup, run `cmake --install build --prefix <dir>`, then
`gtiup link dev <dir>` and `gtiup default dev` (gtiup 0.5.0 or later).
