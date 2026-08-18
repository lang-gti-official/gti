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

-- The channel is read here rather than from the plugin spec's `opts`, because
-- lazy.nvim routes `opts` to the plugin's setup function and never to a build
-- step. `vim.g` is set before the spec loads and is visible to this script.
local channel = vim.g.gti_channel or vim.env.GTI_CHANNEL or "stable"
if channel ~= "stable" and channel ~= "nightly" then
  error(string.format(
    "vim.g.gti_channel must be \"stable\" or \"nightly\", got %q",
    tostring(channel)
  ))
end

installer.install({
  root = root,
  channel = channel,
  on_progress = report,
})
