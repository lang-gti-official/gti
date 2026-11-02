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
  local original_build = string.rep("a", 40)
  local replacement_build = string.rep("b", 40)
  file(projects.nightly .. "/share/gti/BUILD_ID", { original_build })
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
  vim.env.GTI_PATH = projects.stable .. "/bin/gti"
  assert(toolchain.parser({ root = temporary, cwd = temporary }) == nil,
    "compiler-only override must not hide a failed managed server/parser selection")
  vim.env.GTI_PATH = nil
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
  local launch_configs = {}
  for _, project in ipairs({ projects.nightly, projects.stable }) do
    local launch_config = { cmd = config.cmd, root_dir = project, settings = {} }
    config.cmd({}, launch_config)
    table.insert(launch_configs, launch_config)
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
  local autocmds = {}
  local report
  local selected_client = {
    id = 17, name = "gti_lsp", server_info = { version = "1.2.3" }, settings = {},
    config = launch_configs[1],
  }
  local other_client = {
    id = 18, name = "gti_lsp", server_info = { version = "9.9.9" }, settings = {},
    config = launch_configs[2],
  }
  local original_buffer = vim.api.nvim_get_current_buf()
  local info_buffer = vim.api.nvim_create_buf(false, true)
  vim.bo[info_buffer].buftype = ""
  vim.fn.mkdir(projects.stable .. "/src", "p")
  vim.api.nvim_buf_set_name(info_buffer, projects.stable .. "/src/example.gti")
  vim.api.nvim_set_current_buf(info_buffer)
  replace(vim.g, "loaded_gti", nil)
  replace(vim.api, "nvim_create_user_command", function(name, callback) commands[name] = callback end)
  replace(vim.api, "nvim_create_autocmd", function(event, opts)
    autocmds[event] = opts.callback
    return 1
  end)
  replace(vim.api, "nvim_create_augroup", function() return 1 end)
  replace(vim.api, "nvim_set_hl", function() end)
  replace(vim.filetype, "add", function() end)
  replace(vim.lsp, "enable", function() end)
  replace(package.loaded, "gti.treesitter", { setup = function() return true end })
  replace(vim.lsp, "get_clients", function(filter)
    if filter.bufnr == info_buffer then return { selected_client } end
    return { selected_client, other_client }
  end)
  replace(vim.lsp, "get_client_by_id", function(id)
    return id == selected_client.id and selected_client or other_client
  end)
  local system = vim.system
  local probes = 0
  replace(vim, "system", function(...)
    probes = probes + 1
    return system(...)
  end)
  vim.notify = function(message) report = message end
  dofile(repository .. "/plugin/gti.lua")
  local startup_finished = false
  vim.schedule(function() startup_finished = true end)
  assert(vim.wait(1000, function() return startup_finished end))
  assert(autocmds.LspAttach, "plugin must record every attached server")
  autocmds.LspAttach({ data = { client_id = selected_client.id } })
  assert(probes == 0, "managed startup/attachment must not run new manager/version probes")
  assert(report == nil, "coherent managed server must not be compared with plugin VERSION")
  commands.GTIInfo()
  assert(report:find(projects.nightly .. "/bin/gti_lsp", 1, true),
    "GTIInfo must prefer the current buffer's attached client root")
  assert(not report:find("9.9.9", 1, true), "GTIInfo must not report other projects' clients")
  assert(not report:find("Configured channel:", 1, true),
    "managed selection must not display the legacy installer's channel")
  local running = assert(report:match("Running LSP:[^\n]*"))
  assert(running:find(projects.nightly .. "/bin/gti_lsp", 1, true)
    and running:find(original_build, 1, true),
    "function-valued launch must retain its actual command and build identity")
  -- A nightly update keeps VERSION stable. Status must distinguish the newly
  -- selected install from the client launched before its files were replaced.
  file(projects.nightly .. "/share/gti/BUILD_ID", { replacement_build })
  commands.GTIInfo()
  running = assert(report:match("Running LSP:[^\n]*"))
  assert(running:find(original_build, 1, true) and not running:find(replacement_build, 1, true),
    "running build identity was reread after launch")
  assert(report:find(replacement_build, 1, true) and report:find("different builds", 1, true),
    "status must report stale running builds even when semantic versions match")
  vim.env.GTI_PATH = projects.stable .. "/bin/gti"
  vim.env.GTI_LSP_PATH = projects.stable .. "/bin/gti_lsp"
  commands.GTIInfo()
  assert(report:match("Compiler:[^\n]*"):find(vim.env.GTI_PATH, 1, true)
    and report:match("Language server:[^\n]*"):find(vim.env.GTI_LSP_PATH, 1, true),
    "project-scoped status must still honor explicit executable overrides")
  assert(report:match("Running LSP:[^\n]*"):find(projects.nightly .. "/bin/gti_lsp", 1, true),
    "changed overrides must not hide the previous running server")
  local overridden = { cmd = config.cmd, root_dir = projects.nightly }
  config.cmd({}, overridden)
  assert(calls[#calls].cmd[1] == vim.env.GTI_LSP_PATH,
    "launch-time project selection must preserve an explicit server override")
  vim.env.GTI_PATH = nil
  vim.env.GTI_LSP_PATH = nil
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
