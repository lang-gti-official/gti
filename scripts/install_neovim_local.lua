-- The Python entry point stages and tests this exact local archive first.
assert(vim.fn.has("nvim-0.11") == 1, "Neovim 0.11+ is required")
if arg[1] == "probe" then
  io.write(vim.json.encode(vim.fn.stdpath("config")))
  return
end
local root = assert(vim.env.GTI_TEST_REPOSITORY)
local archive = assert(vim.env.GTI_TEST_ARCHIVE)
local checksum = assert(vim.env.GTI_TEST_CHECKSUM)
local platform = assert(vim.env.GTI_TEST_PLATFORM)
local installer = dofile(vim.fs.joinpath(root, "lua", "gti", "installer.lua"))
installer.install({
  root = root,
  platform = platform,
  archive_path = archive,
  checksum_path = checksum,
  -- Local edits can change binaries without changing VERSION.
  force = true,
  on_progress = function(message) print(message) end,
})
