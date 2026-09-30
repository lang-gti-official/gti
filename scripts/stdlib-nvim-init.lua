local root = vim.env.GTI_DEV_ROOT
if not root or root == "" then
  error("GTI_DEV_ROOT is required; launch Neovim with scripts/stdlib-nvim")
end

-- The normal configuration may contain the installed GTI plugin. Prevent only
-- that plugin from initializing while the rest of the user's setup loads.
vim.g.loaded_gti = true
local user_init = vim.fn.stdpath("config") .. "/init.lua"
if vim.uv.fs_stat(user_init) then
  dofile(user_init)
end

local build = vim.env.GTI_STDLIB_BUILD_DIR
vim.env.GTI_STDLIB_PATH = root .. "/stdlib"
vim.env.GTI_RUNTIME_INCLUDE = root .. "/runtime/include"
vim.env.GTI_RUNTIME_LIBRARY = build .. "/libgti_runtime.a"
vim.env.GTI_VENDOR_INCLUDE = root .. "/vendor/expected_lite/include"
vim.env.GTI_PATH = build .. "/gti"
vim.env.GTI_LSP_PATH = build .. "/gti_lsp"
vim.env.GTI_TREE_SITTER_PATH = build .. "/gti.so"
vim.g.gti_channel = "nightly"

vim.opt.runtimepath:prepend(root)
for _, module in ipairs({ "gti.toolchain", "gti.treesitter", "gti.installer" }) do
  package.loaded[module] = nil
end
vim.g.loaded_gti = nil
dofile(root .. "/plugin/gti.lua")
