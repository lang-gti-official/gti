Neovim plugins that attach from their own `FileType` handlers, such as rainbow
delimiters, work again in GTI buffers when the toolchain is managed by gtiup:
the project grammar is now registered before `FileType` runs.
