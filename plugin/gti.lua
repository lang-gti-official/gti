if vim.g.loaded_gti then
  return
end
vim.g.loaded_gti = true

vim.filetype.add({
  extension = {
    gti = "gti",
  },
})

local toolchain = require("gti.toolchain")
toolchain.prepend_path()

if vim.lsp and vim.lsp.enable then
  vim.lsp.enable("gti_lsp")
else
  vim.schedule(function()
    vim.notify("GTI requires Neovim 0.11 or newer for native LSP configuration", vim.log.levels.ERROR)
  end)
end

for group, target in pairs({
  ["@lsp.type.namespace.gti"] = "@module",
  ["@lsp.type.class.gti"] = "@type",
  ["@lsp.type.method.gti"] = "@function.method",
  ["@lsp.type.parameter.gti"] = "@variable.parameter",
  ["@lsp.type.property.gti"] = "@variable.member",
  ["@lsp.type.macro.gti"] = "@constant.macro",
  ["@lsp.type.decorator.gti"] = "@attribute",
  ["@lsp.typemod.variable.readonly.gti"] = "@constant",
  ["@lsp.typemod.parameter.readonly.gti"] = "@constant.parameter",
}) do
  vim.api.nvim_set_hl(0, group, { default = true, link = target })
end

vim.api.nvim_create_user_command("GTIInfo", function()
  local root = toolchain.root()
  local version_path = vim.fs.joinpath(root, "toolchain", "share", "gti", "VERSION")
  local version = "not installed by Lazy"
  if vim.uv.fs_stat(version_path) then
    version = vim.trim(table.concat(vim.fn.readfile(version_path), "\n"))
  end
  vim.notify(table.concat({
    "GTI version: " .. version,
    "Compiler: " .. toolchain.executable("gti"),
    "Language server: " .. toolchain.executable("gti_lsp"),
  }, "\n"), vim.log.levels.INFO, { title = "GTI" })
end, { desc = "Show the active GTI toolchain" })
