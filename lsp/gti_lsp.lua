local toolchain = require("gti.toolchain")

return {
  -- Resolve at client launch, once Neovim has identified the project root.
  -- A single editor can therefore use different selected LSPs for two projects.
  cmd = function(dispatchers, config)
    local root = config.root_dir or vim.uv.cwd()
    local executable = toolchain.executable("gti_lsp", { cwd = root })
    local launch = { command = { executable }, binary = toolchain.binary_info(executable) }
    if toolchain.managed() then
      local compiler = vim.env.GTI_PATH
      if (not compiler or compiler == "")
        and (not vim.env.GTI_LSP_PATH or vim.env.GTI_LSP_PATH == "")
      then
        -- A manager-selected release contains the compiler beside its server.
        -- With a server override only, the manager's compiler is unknown until
        -- GTIInfo resolves it; never substitute the overridden server's sibling.
        compiler = vim.fs.joinpath(vim.fs.dirname(launch.binary.resolved_path), "gti")
      end
      launch.compiler = toolchain.binary_info(compiler)
    end
    config._gti_toolchain_launch = launch
    return vim.lsp.rpc.start(launch.command, dispatchers, {
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
