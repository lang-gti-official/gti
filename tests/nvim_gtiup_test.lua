-- Offline selection regressions; fake toolchains require no compiler build.
local repository = assert(vim.env.GTI_TEST_REPOSITORY)
local temporary = vim.fn.tempname()
vim.fn.mkdir(temporary, "p")
temporary = vim.uv.fs_realpath(temporary)
local saved_path = vim.env.PATH
local saved_manager = vim.env.GTIUP_PATH
local saved_compiler = vim.env.GTI_PATH
local saved_server = vim.env.GTI_LSP_PATH
local saved_parser = vim.env.GTI_TREE_SITTER_PATH
local saved_mode = vim.g.gti_toolchain
local saved_rpc = vim.lsp.rpc.start
local saved_language_add = vim.treesitter.language.add
local saved_query_get = vim.treesitter.query.get
local saved_notify = vim.notify
local saved_module = package.loaded["gti.toolchain"]
local replaced = {}
local function replace(object, key, value)
  table.insert(replaced, { object, key, object[key] })
  object[key] = value
end
local ok, problem = xpcall(function()
  local function file(path, lines, executable)
    vim.fn.mkdir(vim.fs.dirname(path), "p")
    vim.fn.writefile(lines, path)
    if executable then vim.uv.fs_chmod(path, 493) end
  end
  local projects = {}
  for _, channel in ipairs({ "nightly", "stable" }) do
    local prefix = vim.fs.joinpath(temporary, channel)
    projects[channel] = prefix
    file(prefix .. "/bin/gti", { "#!/bin/sh", "echo 'gti 1.2.3'" }, true)
    file(prefix .. "/bin/gti_lsp", { "#!/bin/sh", "echo 'gti_lsp 1.2.3'" }, true)
    file(prefix .. "/share/gti/parser/gti.so", { "fake parser" })
    file(prefix .. "/share/gti/VERSION", { "1.2.3" })
    file(prefix .. "/gti-toolchain", { channel })
  end
  -- Deliberately executable legacy tools must never win over managed selection.
  file(temporary .. "/toolchain/bin/gti", { "#!/bin/sh", "exit 9" }, true)
  file(temporary .. "/toolchain/bin/gti_lsp", { "#!/bin/sh", "exit 9" }, true)
  file(temporary .. "/toolchain/share/gti/parser/gti.so", { "stale parser" })
  local manager = temporary .. "/gtiup"
  file(manager, {
    "#!/bin/sh",
    '[ "$1" = which ] || exit 9',
    'selected="$PWD"',
    'while [ ! -f "$selected/gti-toolchain" ]; do',
    '  [ "$selected" = / ] && { echo "no selected toolchain" >&2; exit 1; }',
    '  selected=${selected%/*}; [ -n "$selected" ] || selected=/',
    'done',
    'printf "%s/bin/%s\\n" "$selected" "$2"',
  }, true)
  vim.env.GTIUP_PATH = manager
  vim.env.GTI_PATH = nil
  vim.env.GTI_LSP_PATH = nil
  vim.env.GTI_TREE_SITTER_PATH = nil
  vim.g.gti_toolchain = "gtiup"
  local toolchain = dofile(repository .. "/lua/gti/toolchain.lua")
  package.loaded["gti.toolchain"] = toolchain
  assert(toolchain.managed())
  for _, project in pairs(projects) do
    local opts = { root = temporary, cwd = project }
    assert(toolchain.executable("gti", opts) == project .. "/bin/gti")
    assert(toolchain.executable("gti_lsp", opts) == project .. "/bin/gti_lsp")
    assert(toolchain.parser(opts) == project .. "/share/gti/parser/gti.so")
    local info = toolchain.version_info({ root = temporary, cwd = project, expected_version = "9.9.9" })
    assert(info.managed and info.installed == "1.2.3" and #info.problems == 0,
      "managed toolchains must not be compared with the plugin checkout VERSION")
  end
  assert(not toolchain.prepend_path(temporary) and vim.env.PATH == saved_path,
    "managed mode must not prepend the old plugin installation")
  local resolved, message = pcall(toolchain.executable, "gti", { root = temporary, cwd = temporary })
  assert(not resolved and message:find("no selected toolchain", 1, true), "selection must fail closed")
  assert(toolchain.parser({ root = temporary, cwd = temporary }) == nil)
  local missing = toolchain.version_info({ cwd = temporary, probe_binaries = false })
  assert(#missing.problems == 2 and missing.compiler == "unavailable")
  vim.env.GTIUP_PATH = temporary .. "/missing-gtiup"
  local launched, launch_problem = pcall(toolchain.executable, "gti", { cwd = projects.stable })
  assert(not launched and launch_problem:find("cannot run", 1, true)
    and launch_problem:find("GTIUP_PATH", 1, true), "missing manager needs an actionable diagnostic")
  vim.env.GTIUP_PATH = manager
  vim.env.GTI_PATH = projects.stable .. "/bin/gti"
  vim.env.GTI_LSP_PATH = projects.stable .. "/bin/gti_lsp"
  vim.env.GTI_TREE_SITTER_PATH = projects.stable .. "/share/gti/parser/gti.so"
  assert(toolchain.executable("gti", { cwd = projects.nightly }) == vim.env.GTI_PATH)
  assert(toolchain.executable("gti_lsp", { cwd = projects.nightly }) == vim.env.GTI_LSP_PATH)
  assert(toolchain.parser({ cwd = projects.nightly }) == vim.env.GTI_TREE_SITTER_PATH)
  vim.env.GTI_PATH = nil
  vim.env.GTI_LSP_PATH = nil
  vim.env.GTI_TREE_SITTER_PATH = nil
  local calls = {}
  vim.lsp.rpc.start = function(cmd, _, opts)
    table.insert(calls, { cmd = cmd, cwd = opts.cwd })
    return {}
  end
  local config = dofile(repository .. "/lsp/gti_lsp.lua")
  for _, project in ipairs({ projects.nightly, projects.stable }) do
    config.cmd({}, { root_dir = project })
  end
  assert(calls[1].cmd[1] == projects.nightly .. "/bin/gti_lsp")
  assert(calls[2].cmd[1] == projects.stable .. "/bin/gti_lsp")
  assert(calls[2].cwd == projects.stable, "selection must use LSP root, not editor cwd")
  -- Root-marker order is priority order, not nearest-file order in Neovim.
  -- Two nested projects may inherit one workspace toolchain without sharing
  -- an LSP root (and therefore accidentally sharing project configuration).
  for index, project_name in ipairs({ "one", "two" }) do
    local project = projects.nightly .. "/apps/" .. project_name
    file(project .. "/gti.toml", { "manifest-version = 1" })
    file(project .. "/src/main.gti", { "int main() { return 0; }" })
    local root = vim.fs.root(project .. "/src/main.gti", config.root_markers)
    assert(root == project, "ancestor gti-toolchain must not override the nested project manifest")
    config.cmd({}, { root_dir = root })
    assert(calls[index + 2].cwd == project, "nested projects must retain distinct LSP roots")
    assert(calls[index + 2].cmd[1] == projects.nightly .. "/bin/gti_lsp",
      "nested project must still inherit its ancestor's toolchain selection")
  end
  local parser_loads = 0
  local parser_warning
  vim.treesitter.language.add = function()
    parser_loads = parser_loads + 1
    return true
  end
  vim.treesitter.query.get = function() return {} end
  vim.notify = function(message) parser_warning = message end
  local treesitter = dofile(repository .. "/lua/gti/treesitter.lua")
  assert(treesitter.setup(toolchain, { cwd = projects.nightly }))
  assert(treesitter.setup(toolchain, { cwd = projects.nightly }))
  assert(not treesitter.setup(toolchain, { cwd = projects.stable }))
  assert(parser_loads == 1 and parser_warning:find("different or unavailable", 1, true),
    "a second selected grammar must not reuse the first silently")
  -- Exercise the user command itself with two projects and an unrelated
  -- editor cwd. Its selection must agree with the attached client's root.
  local commands = {}
  local report
  local selected_client = {
    name = "gti_lsp", server_info = { version = "1.2.3" }, settings = {},
    config = { root_dir = projects.nightly, settings = {} },
  }
  local other_client = { name = "gti_lsp", server_info = { version = "9.9.9" }, settings = {}, config = {} }
  local original_buffer = vim.api.nvim_get_current_buf()
  local info_buffer = vim.api.nvim_create_buf(false, true)
  vim.bo[info_buffer].buftype = ""
  vim.fn.mkdir(projects.stable .. "/src", "p")
  vim.api.nvim_buf_set_name(info_buffer, projects.stable .. "/src/example.gti")
  vim.api.nvim_set_current_buf(info_buffer)
  replace(vim.g, "loaded_gti", nil)
  replace(vim.api, "nvim_create_user_command", function(name, callback) commands[name] = callback end)
  replace(vim.api, "nvim_create_autocmd", function() return 1 end)
  replace(vim.api, "nvim_create_augroup", function() return 1 end)
  replace(vim.api, "nvim_set_hl", function() end)
  replace(vim.filetype, "add", function() end)
  replace(vim.lsp, "enable", function() end)
  replace(package.loaded, "gti.treesitter", { setup = function() return true end })
  replace(vim.lsp, "get_clients", function(filter)
    if filter.bufnr == info_buffer then return { selected_client } end
    return { selected_client, other_client }
  end)
  vim.notify = function(message) report = message end
  dofile(repository .. "/plugin/gti.lua")
  commands.GTIInfo()
  assert(report:find(projects.nightly .. "/bin/gti_lsp", 1, true),
    "GTIInfo must prefer the current buffer's attached client root")
  assert(not report:find("9.9.9", 1, true), "GTIInfo must not report other projects' clients")
  replace(vim.lsp, "get_clients", function() return {} end)
  commands.GTIInfo()
  assert(report:find(projects.stable .. "/bin/gti_lsp", 1, true),
    "without an attached client GTIInfo must resolve the current buffer's project")
  vim.api.nvim_set_current_buf(original_buffer)
  vim.api.nvim_buf_delete(info_buffer, { force = true })
  -- The managed build hook is a no-op, including when an obsolete channel
  -- option is left over from the previous plugin installer.
  local saved_channel = vim.g.gti_channel
  vim.g.gti_channel = "obsolete-channel"
  dofile(repository .. "/build.lua")
  vim.g.gti_channel = saved_channel
  vim.g.gti_toolchain = nil
  assert(toolchain.executable("gti", { root = temporary }) == temporary .. "/toolchain/bin/gti",
    "legacy mode must remain available")
end, debug.traceback)
for index = #replaced, 1, -1 do
  local entry = replaced[index]
  entry[1][entry[2]] = entry[3]
end
vim.env.PATH = saved_path
vim.env.GTIUP_PATH = saved_manager
vim.env.GTI_PATH = saved_compiler
vim.env.GTI_LSP_PATH = saved_server
vim.env.GTI_TREE_SITTER_PATH = saved_parser
vim.g.gti_toolchain = saved_mode
vim.lsp.rpc.start = saved_rpc
vim.treesitter.language.add = saved_language_add
vim.treesitter.query.get = saved_query_get
vim.notify = saved_notify
package.loaded["gti.toolchain"] = saved_module
vim.fn.delete(temporary, "rf")
assert(ok, problem)
