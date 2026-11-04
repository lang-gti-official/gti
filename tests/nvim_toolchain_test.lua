-- Toolchain selection/reporting only; no user config or real compiler is loaded.
if vim.fn.has("nvim-0.11") == 0 then
  vim.cmd("cquit 77")
end

local repository = assert(vim.env.GTI_TEST_REPOSITORY)
local temporary = vim.fn.tempname()
local notifications = {}
local original_notify = vim.notify
local original_get_clients = vim.lsp.get_clients
local original_get_client = vim.lsp.get_client_by_id
local original_enable = vim.lsp.enable
local original_system = vim.system

local function write(path, text)
  vim.fn.mkdir(vim.fs.dirname(path), "p")
  vim.fn.writefile(vim.split(text, "\n", { plain = true }), path)
end

local function installation(prefix, version, marker, identity)
  local bin = vim.fs.joinpath(prefix, "bin")
  for _, name in ipairs({ "gti", "gti_lsp" }) do
    local path = vim.fs.joinpath(bin, name)
    write(path, "#!/bin/sh\nprintf '" .. name .. " " .. version .. "\\n'\n")
    assert(vim.uv.fs_chmod(path, 493))
  end
  write(vim.fs.joinpath(prefix, "share/gti/VERSION"), version)
  write(vim.fs.joinpath(prefix, "share/gti/parser/gti.so"), "fixture")
  if marker then write(vim.fs.joinpath(prefix, "share/gti", marker), identity) end
  return bin
end

local ok, problem = xpcall(function()
  vim.opt.runtimepath:prepend(repository)
  local toolchain = require("gti.toolchain")
  local plugin_root = vim.fs.joinpath(temporary, "plugin")
  write(vim.fs.joinpath(plugin_root, "VERSION"), "0.2.0")
  installation(vim.fs.joinpath(plugin_root, "toolchain"), "0.2.0")
  local external = vim.fs.joinpath(temporary, "external")
  local original_build = string.rep("a", 40)
  local replacement_build = string.rep("b", 40)
  local external_bin = installation(external, "0.1.0", "PREVIEW_SOURCE", original_build)
  local external_parser = vim.fs.joinpath(external, "share/gti/parser/gti.so")
  local server_link = vim.fs.joinpath(temporary, "commands/gti_lsp")
  vim.fn.mkdir(vim.fs.dirname(server_link), "p")
  assert(vim.uv.fs_symlink(vim.fs.joinpath(external_bin, "gti_lsp"), server_link))
  vim.env.GTI_PATH = vim.fs.joinpath(external_bin, "gti")
  vim.env.GTI_LSP_PATH = server_link
  vim.env.GTI_TREE_SITTER_PATH = nil
  assert(toolchain.parser({ root = plugin_root }) == vim.uv.fs_realpath(external_parser),
    "explicit language-server toolchain must select its sibling parser before the bundled parser")

  local override_parser = vim.fs.joinpath(temporary, "explicit-parser.so")
  write(override_parser, "fixture")
  vim.env.GTI_TREE_SITTER_PATH = override_parser
  assert(toolchain.parser({ root = plugin_root }) == override_parser,
    "explicit parser override lost priority")
  vim.env.GTI_TREE_SITTER_PATH = nil

  vim.g.gti_channel = "nightly"
  local info = toolchain.version_info({ root = plugin_root })
  assert(#info.problems == 0, "coherent rolling build must not be compared to the checkout VERSION: "
    .. vim.inspect(info.problems))
  assert(info.compiler_info.build_id == original_build and info.compiler_info.channel == "preview")
  vim.g.gti_channel = "stable"
  assert(#toolchain.version_info({ root = plugin_root }).problems > 0,
    "stable toolchain/plugin version mismatch was hidden")
  write(vim.fs.joinpath(plugin_root, "VERSION"), "0.1.0")
  assert(#toolchain.version_info({ root = plugin_root }).problems == 0,
    "unused bundled toolchain caused a false stable mismatch")
  write(vim.fs.joinpath(plugin_root, "VERSION"), "0.2.0")
  vim.g.gti_channel = "nightly"
  local mismatched_bin = installation(vim.fs.joinpath(temporary, "mismatched"), "0.3.0",
    "BUILD_ID", replacement_build)
  vim.env.GTI_PATH = vim.fs.joinpath(mismatched_bin, "gti")
  assert(#toolchain.version_info({ root = plugin_root }).problems > 0,
    "rolling compiler/server version mismatch was hidden")
  installation(vim.fs.dirname(mismatched_bin), "0.1.0", "BUILD_ID", replacement_build)
  assert(#toolchain.version_info({ root = plugin_root }).problems > 0,
    "different builds with the same semantic version were treated as coherent")
  vim.env.GTI_PATH = vim.fs.joinpath(external_bin, "gti")

  -- Loading the real plugin exercises its attachment warning and GTIInfo paths.
  -- Parser compatibility is independently tested with the real parser; avoid
  -- passing this intentionally fake fixture to Tree-sitter.
  toolchain.root = function() return plugin_root end
  package.loaded["gti.treesitter"] = { setup = function() end }
  vim.notify = function(message) table.insert(notifications, message) end
  vim.lsp.enable = function() end
  local client = {
    id = 17, name = "gti_lsp", server_info = { version = "0.1.0" }, settings = {},
    config = { cmd = { vim.env.GTI_LSP_PATH }, root_dir = temporary },
  }
  vim.lsp.get_clients = function() return { client } end
  vim.lsp.get_client_by_id = function(id) return id == client.id and client or nil end
  local probes = 0
  vim.system = function(...)
    probes = probes + 1
    return original_system(...)
  end
  vim.cmd("runtime plugin/gti.lua")
  local startup_finished = false
  vim.schedule(function() startup_finished = true end)
  assert(vim.wait(1000, function() return startup_finished end))
  vim.api.nvim_exec_autocmds("LspAttach", { data = { client_id = client.id } })
  assert(probes == 0, "startup/attachment blocked on a process probe")
  assert(#notifications == 0, "rolling LspAttach emitted a false version mismatch: "
    .. vim.inspect(notifications))

  -- Change the selected install and the old install's on-disk marker after
  -- attachment. Status must retain the running client's launch identity.
  vim.env.GTI_LSP_PATH = vim.fs.joinpath(mismatched_bin, "gti_lsp")
  vim.env.GTI_PATH = vim.fs.joinpath(mismatched_bin, "gti")
  write(vim.fs.joinpath(external, "share/gti/PREVIEW_SOURCE"), replacement_build)
  vim.cmd("GTIInfo")
  local status = notifications[#notifications]
  assert(status:find(client.config.cmd[1], 1, true), "status hides the running server command")
  assert(status:find(vim.uv.fs_realpath(vim.fs.joinpath(external_bin, "gti_lsp")), 1, true),
    "status hides the running executable behind its command symlink")
  assert(status:find(original_build, 1, true), "status lost the build identity captured at attachment")
  assert(status:find(replacement_build, 1, true), "status hides the newly selected build")
  assert(status:find("nightly", 1, true) and status:find("preview", 1, true),
    "status hides the selected/running channels")
  assert(status:find("different builds", 1, true), "status hides a stale running build")
end, debug.traceback)

vim.notify = original_notify
vim.lsp.get_clients = original_get_clients
vim.lsp.get_client_by_id = original_get_client
vim.lsp.enable = original_enable
vim.system = original_system
vim.fn.delete(temporary, "rf")
if not ok then
  io.stderr:write(tostring(problem) .. "\n")
  vim.cmd("cquit 1")
end
print("GTI Neovim toolchain selection/status regression passed")
vim.cmd("qa!")
