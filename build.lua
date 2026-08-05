local source = debug.getinfo(1, "S").source
local path = source:sub(1, 1) == "@" and source:sub(2) or source
local root = vim.fs.dirname(vim.uv.fs_realpath(path) or path)
local installer = dofile(vim.fs.joinpath(root, "lua", "gti", "installer.lua"))

local function report(message)
  local running, is_main = coroutine.running()
  if running and not is_main then
    coroutine.yield({ msg = message, level = vim.log.levels.TRACE })
  else
    vim.notify(message, vim.log.levels.INFO, { title = "GTI" })
  end
end

installer.install({
  root = root,
  on_progress = report,
})
