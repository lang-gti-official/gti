-- Isolated client/server regression; never loads the user's editor config.
if vim.fn.has("nvim-0.11") == 0 then
  vim.cmd("cquit 77")
end

local repository = assert(vim.env.GTI_TEST_REPOSITORY)
local server = assert(vim.env.GTI_TEST_LSP)
local compiler = assert(vim.env.GTI_TEST_COMPILER)
local temporary = vim.fn.tempname()
vim.fn.mkdir(vim.fs.joinpath(temporary, "src"), "p")
local client

local ok, problem = xpcall(function()
  vim.opt.runtimepath:prepend(repository)
  vim.env.GTI_PATH = compiler
  vim.env.GTI_LSP_PATH = server
  vim.g.gti_channel = "nightly"
  vim.lsp.config("gti_lsp", { settings = { gti = { profile = "release" } } })
  vim.cmd("filetype plugin on")
  vim.cmd("syntax on")
  vim.cmd("runtime plugin/gti.lua")

  local inactive_group = "@lsp.mod.inactiveCode.gti"
  local function highlight()
    return vim.api.nvim_get_hl(0, { name = inactive_group, link = true })
  end
  assert(highlight().link == "Comment", "missing inactive-code default")
  vim.cmd("colorscheme default")
  assert(highlight().link == "Comment", "colorscheme cleared inactive-code style")
  vim.api.nvim_set_hl(0, inactive_group, { fg = 0x445566 })
  vim.api.nvim_exec_autocmds("ColorScheme", {})
  assert(highlight().fg == 0x445566, "plugin overwrote a custom inactive-code style")

  vim.fn.writefile({
    'manifest-version = 1', '[package]', 'name = "editor"', 'version = "0.1.0"',
    '[targets.editor]', 'kind = "executable"', 'root = "src/main.gti"',
    '[profiles.dev]', 'defines = ["PSYCH_DEBUG"]', '[profiles.release]', 'defines = []',
  }, vim.fs.joinpath(temporary, "gti.toml"))
  local path = vim.fs.joinpath(temporary, "src", "main.gti")
  vim.fn.writefile({
    '#ifdef PSYCH_DEBUG',
    'int dev_branch() { return 0; }',
    '#else',
    'int release_branch() { return 0; }',
    '#endif',
    'int main() { return 0; }',
  }, path)
  vim.cmd("edit " .. vim.fn.fnameescape(path))
  local buffer = vim.api.nvim_get_current_buf()
  assert(vim.wait(10000, function()
    local clients = vim.lsp.get_clients({ bufnr = buffer, name = "gti_lsp" })
    client = clients[1]
    return client and client.initialized
  end, 20), "GTI server did not attach")
  local client_id = client.id
  local legend = client.server_capabilities.semanticTokensProvider.legend
  local mask
  for index, modifier in ipairs(legend.tokenModifiers) do
    if modifier == "inactiveCode" then mask = 2 ^ (index - 1) end
  end
  assert(mask, "inactiveCode not advertised")

  local function assert_profile(debug)
    local response = assert(client:request_sync("textDocument/semanticTokens/full", {
      textDocument = { uri = vim.uri_from_bufnr(buffer) },
    }, 10000, buffer))
    assert(not response.err, vim.inspect(response.err))
    local line, column = 0, 0
    local names = {}
    for index = 1, #response.result.data, 5 do
      local data = response.result.data
      if data[index] == 0 then column = column + data[index + 1]
      else line, column = line + data[index], data[index + 1] end
      if column == 4 and (line == 1 or line == 3) then
        names[line] = bit.band(data[index + 4], mask) ~= 0
      end
    end
    assert(names[1] == not debug and names[3] == debug, vim.inspect(names))
  end
  assert_profile(false) -- Persistent client settings apply on first open.
  vim.cmd("GTIProfile dev")
  assert_profile(true)
  assert(client.id == client_id, "profile switch unnecessarily restarted server")
  assert(client.settings.gti.profile == "dev")
  vim.cmd("GTIProfile release")
  assert_profile(false)

  -- Verify the inactive overlay wins above all normal type+modifier styles.
  local token
  assert(vim.wait(10000, function()
    vim.cmd("redraw")
    for _, candidate in ipairs(vim.lsp.semantic_tokens.get_at_pos(buffer, 1, 4) or {}) do
      if candidate.modifiers.inactiveCode then token = candidate; return true end
    end
    return false
  end, 20), "Neovim did not receive refreshed inactive tokens")
  vim.api.nvim_exec_autocmds("LspTokenUpdate", {
    buffer = buffer, data = { client_id = client_id, token = token },
  })
  local function inactive_marks(row)
    local result = {}
    for name, namespace in pairs(vim.api.nvim_get_namespaces()) do
      if name:find("nvim.lsp.semantic_tokens", 1, true) then
        for _, mark in ipairs(vim.api.nvim_buf_get_extmarks(buffer, namespace, { row, 0 }, { row, -1 }, { details = true })) do
          if mark[4].hl_group == inactive_group then result[#result + 1] = mark end
        end
      end
    end
    return result
  end
  local high_priority = false
  for _, mark in ipairs(inactive_marks(1)) do
    if mark[4].priority >= vim.hl.priorities.semantic_tokens + 3 then high_priority = true end
  end
  assert(high_priority, "inactive code does not override normal semantic colours")
  vim.cmd("GTIProfile dev")
  assert_profile(true)
  assert(vim.wait(10000, function()
    vim.cmd("redraw")
    return #inactive_marks(1) == 0
  end, 20), "old inactive-code overlays survived the profile switch")
  vim.cmd("GTIProfile!")
  assert(client.settings.gti.profile == vim.NIL)
  assert_profile(true)

  vim.api.nvim_buf_set_lines(buffer, 0, -1, false, {
    'void use(int value) {}',
    '#macro inspect(int value) { mut int local_value = value; use(local_value); }',
    'int main() { inspect(7); return 0; }',
  })
  vim.api.nvim_win_set_cursor(0, { 3, 15 })
  local unchanged = vim.api.nvim_buf_get_changedtick(buffer)
  vim.cmd("GTIExpandMacro")
  assert(vim.wait(10000, function()
    return vim.api.nvim_get_current_buf() ~= buffer and vim.b.gti_macro_expansion
  end, 20), "macro inspection did not open a scratch view")
  local expansion = vim.api.nvim_get_current_buf()
  assert(vim.bo[expansion].buftype == "nofile" and vim.bo[expansion].readonly
    and not vim.bo[expansion].modifiable, "macro view is not read-only")
  local text = table.concat(vim.api.nvim_buf_get_lines(expansion, 0, -1, false), "\n")
  assert(text:find("not executable source", 1, true), text)
  assert(text:find("local_value__macro_local_1", 1, true), text)
  assert(vim.api.nvim_buf_get_changedtick(buffer) == unchanged, "inspection edited source")
  assert(not pcall(vim.api.nvim_buf_set_lines, expansion, 0, -1, false, { "changed" }),
    "inspection scratch accepts accidental source edits")
  vim.cmd("close")
  assert(vim.api.nvim_get_current_buf() == buffer)

  -- Simulate an editor response arriving after a source edit. The command
  -- must discard it before opening another explanatory buffer.
  local request = client.request
  local delayed
  client.request = function(self, method, params, callback, request_buffer)
    if method == "gti/inspectMacroExpansion" then delayed = callback; return true, 99999 end
    return request(self, method, params, callback, request_buffer)
  end
  vim.cmd("GTIExpandMacro")
  client.request = request
  assert(delayed, "macro command did not use the documented request")
  vim.api.nvim_buf_set_lines(buffer, 0, 0, false, { "// edited while inspecting" })
  delayed(nil, { readOnly = true, text = "// obsolete expansion" })
  assert(vim.api.nvim_get_current_buf() == buffer, "stale inspection opened a view")
end, debug.traceback)

if client then
  client:stop(true)
  vim.wait(3000, function() return client:is_stopped() end, 20)
end
vim.fn.delete(temporary, "rf")
if not ok then
  io.stderr:write(tostring(problem) .. "\n")
  vim.cmd("cquit 1")
end
print("GTI Neovim profile/highlighting regression passed")
vim.cmd("qa!")
