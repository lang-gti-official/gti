local toolchain = require("gti.toolchain")

return {
  -- Resolve at client launch, once Neovim has identified the project root.
  -- A single editor can therefore use different selected LSPs for two projects.
  cmd = function(dispatchers, config)
    local root = config.root_dir or vim.uv.cwd()
    return vim.lsp.rpc.start({ toolchain.executable("gti_lsp", { cwd = root }) }, dispatchers, {
      cwd = root,
      env = config.cmd_env,
      detached = config.detached,
    })
  end,
  filetypes = { "gti" },
  -- Toolchain selection is inherited; it must not replace a nearer project
  -- manifest with an ancestor workspace root. Preserve existing marker priority.
  root_markers = { "gti.toml", ".git", "CMakeLists.txt", "gti-toolchain" },
}
