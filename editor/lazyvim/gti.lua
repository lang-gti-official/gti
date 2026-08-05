vim.filetype.add({
  extension = {
    gti = "gti",
  },
})

local function server_command()
  if vim.env.GTI_LSP_PATH and vim.env.GTI_LSP_PATH ~= "" then
    return vim.env.GTI_LSP_PATH
  end

  local path_command = vim.fn.exepath("gti_lsp")
  if path_command ~= "" then
    return path_command
  end

  local source = debug.getinfo(1, "S").source
  if source:sub(1, 1) == "@" then
    local real_source = vim.uv.fs_realpath(source:sub(2))
    if real_source then
      local repository_server = vim.fs.normalize(vim.fs.joinpath(
        vim.fs.dirname(real_source),
        "..",
        "..",
        "build",
        "gti_lsp"
      ))
      if vim.fn.executable(repository_server) == 1 then
        return repository_server
      end
    end
  end

  return "gti_lsp"
end

vim.lsp.config("gti_lsp", {
  cmd = { server_command() },
  filetypes = { "gti" },
  root_markers = { ".git", "CMakeLists.txt" },
})

vim.lsp.enable("gti_lsp")

return {
  {
    "nvim-mini/mini.icons",
    opts = function(_, opts)
      opts = opts or {}
      opts.extension = opts.extension or {}
      opts.filetype = opts.filetype or {}

      local icon = { glyph = "󰬎", hl = "MiniIconsBlue" }
      opts.extension.gti = icon
      opts.filetype.gti = icon

      return opts
    end,
  },
}
