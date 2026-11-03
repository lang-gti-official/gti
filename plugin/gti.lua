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
require("gti.treesitter").setup(toolchain)

-- On the nightly channel the checkout runs ahead of the last published build,
-- so its VERSION legitimately differs from the installed toolchain's. Only the
-- stable channel pins the two together.
local channel = vim.g.gti_channel or vim.env.GTI_CHANNEL or "stable"
local expected_version = toolchain.expected_version()
local installed_version = toolchain.installed_version()
if
  not toolchain.managed()
  and channel == "stable"
  and expected_version
  and installed_version
  and expected_version ~= installed_version
then
  vim.schedule(function()
    vim.notify(
      string.format(
        "GTI plugin %s is using toolchain %s. Run :Lazy build gti, then restart Neovim.",
        expected_version,
        installed_version
      ),
      vim.log.levels.WARN,
      { title = "GTI version mismatch" }
    )
  end)
end

if vim.lsp and vim.lsp.enable then
  vim.lsp.enable("gti_lsp")
else
  vim.schedule(function()
    vim.notify("GTI requires Neovim 0.11 or newer for native LSP configuration", vim.log.levels.ERROR)
  end)
end

local reported_lsp_versions = {}
vim.api.nvim_create_autocmd("LspAttach", {
  group = vim.api.nvim_create_augroup("gti_version_check", { clear = true }),
  callback = function(args)
    local client_id = args.data and args.data.client_id or nil
    local client = client_id and vim.lsp.get_client_by_id(client_id) or nil
    if toolchain.managed() or not client or client.name ~= "gti_lsp" or not expected_version then
      return
    end
    local actual = client.server_info and client.server_info.version or nil
    if not actual or actual == expected_version or reported_lsp_versions[actual] then
      return
    end
    reported_lsp_versions[actual] = true
    vim.notify(
      string.format(
        "gti_lsp %s is attached, but this GTI plugin expects %s. Run :Lazy build gti, then restart Neovim.",
        actual,
        expected_version
      ),
      vim.log.levels.WARN,
      { title = "GTI version mismatch" }
    )
  end,
})

for group, target in pairs({
  ["@lsp.type.keyword.gti"] = "@keyword",
  ["@lsp.type.type.gti"] = "@type",
  ["@lsp.type.typeParameter.gti"] = "@type.definition",
  ["@lsp.type.namespace.gti"] = "@module",
  ["@lsp.type.class.gti"] = "@type",
  ["@lsp.type.function.gti"] = "@function",
  ["@lsp.type.method.gti"] = "@function.method",
  ["@lsp.type.variable.gti"] = "@variable",
  ["@lsp.type.parameter.gti"] = "@variable.parameter",
  ["@lsp.type.property.gti"] = "@variable.member",
  ["@lsp.type.string.gti"] = "@string",
  ["@lsp.type.number.gti"] = "@number",
  ["@lsp.type.operator.gti"] = "@operator",
  ["@lsp.type.macro.gti"] = "@lsp.type.macro",
  ["@lsp.type.decorator.gti"] = "@attribute",
  ["@lsp.type.comment.gti"] = "@comment",
  ["@lsp.type.enumMember.gti"] = "@constant",
  ["@lsp.typemod.namespace.defaultLibrary.gti"] = "@module.builtin",
  ["@lsp.typemod.function.defaultLibrary.gti"] = "@function.builtin",
  ["@lsp.typemod.type.defaultLibrary.gti"] = "@type.builtin",
  ["@lsp.type.struct.gti"] = "@type",
  ["@lsp.type.enum.gti"] = "@type",
  ["@lsp.typemod.class.declaration.gti"] = "@type.definition",
  ["@lsp.typemod.struct.declaration.gti"] = "@type.definition",
  ["@lsp.typemod.enum.declaration.gti"] = "@type.definition",
  ["@lsp.typemod.function.declaration.gti"] = "@function",
  ["@lsp.typemod.method.declaration.gti"] = "@function.method",
  ["@lsp.typemod.function.static.gti"] = "@function",
  ["@lsp.typemod.method.static.gti"] = "@function.method",
  ["@lsp.typemod.variable.static.gti"] = "@variable",
  ["@lsp.typemod.property.static.gti"] = "@variable.member",
  ["@lsp.typemod.variable.readonly.gti"] = "@constant",
  ["@lsp.typemod.parameter.functionScope.gti"] = "@variable.parameter",
  ["@lsp.typemod.parameter.readonly.gti"] = "@constant.parameter",
}) do
  vim.api.nvim_set_hl(0, group, { default = true, link = target })
end

local function inactive_code_highlight()
  -- inactiveCode is a server extension, not a built-in Neovim style. A
  -- language-specific default preserves theme/user overrides and does not
  -- change other languages' semantic token groups.
  vim.api.nvim_set_hl(0, "@lsp.mod.inactiveCode.gti", { default = true, link = "Comment" })
end

inactive_code_highlight()
vim.api.nvim_create_autocmd("ColorScheme", {
  group = vim.api.nvim_create_augroup("gti_inactive_code_style", { clear = true }),
  callback = inactive_code_highlight,
})

vim.api.nvim_create_autocmd("LspTokenUpdate", {
  group = vim.api.nvim_create_augroup("gti_inactive_code_tokens", { clear = true }),
  callback = function(args)
    local data = args.data
    local client = data and vim.lsp.get_client_by_id(data.client_id) or nil
    if not client or client.name ~= "gti_lsp" or not data.token.modifiers.inactiveCode then
      return
    end
    -- Apply above type+modifier styles, which otherwise have higher priority
    -- than a modifier alone. All authority comes from the server's token.
    vim.lsp.semantic_tokens.highlight_token(
      data.token, args.buf, data.client_id, "@lsp.mod.inactiveCode.gti"
    )
  end,
})

local function requested_profile(client)
  local profile
  if type(client.settings.gti) == "table" then
    profile = client.settings.gti.profile
  else
    local options = client.config.init_options
    profile = type(options) == "table" and options.profile or nil
  end
  return type(profile) == "string" and profile or "project default"
end

vim.api.nvim_create_user_command("GTIProfile", function(args)
  local clients = vim.lsp.get_clients({ name = "gti_lsp", bufnr = 0 })
  if #clients == 0 then
    vim.notify("No GTI language server is attached to this buffer", vim.log.levels.WARN, { title = "GTI" })
    return
  end
  if args.args == "" and not args.bang then
    vim.notify("Editor profile: " .. requested_profile(clients[1]), vim.log.levels.INFO, { title = "GTI" })
    return
  end
  if (args.bang and args.args ~= "")
    or (not args.bang and not args.args:match("^[A-Za-z][A-Za-z0-9_-]*$")) then
    vim.notify("Use :GTIProfile <profile-name> or :GTIProfile! to reset", vim.log.levels.ERROR, { title = "GTI" })
    return
  end
  for _, client in ipairs(clients) do
    client.settings.gti = type(client.settings.gti) == "table" and client.settings.gti or {}
    client.settings.gti.profile = args.bang and vim.NIL or args.args
    client:notify("workspace/didChangeConfiguration", { settings = client.settings })
  end
end, {
  nargs = "?",
  bang = true,
  complete = function() return { "dev", "release" } end,
  desc = "Select the GTI editor build profile (without building)",
})

vim.api.nvim_create_user_command("GTIExpandMacro", function()
  local buffer = vim.api.nvim_get_current_buf()
  local client = vim.lsp.get_clients({ name = "gti_lsp", bufnr = buffer })[1]
  local experimental = client and client.server_capabilities.experimental
  if not client or not experimental or not experimental.macroExpansionProvider then
    vim.notify("The attached GTI server does not support macro inspection", vim.log.levels.WARN, { title = "GTI" })
    return
  end
  local generations = {}
  for attached in pairs(client.attached_buffers) do
    if vim.api.nvim_buf_is_valid(attached) then
      generations[attached] = vim.api.nvim_buf_get_changedtick(attached)
    end
  end
  local params = vim.lsp.util.make_position_params(0, client.offset_encoding)
  if vim.lsp.util.buf_versions then
    params.textDocument.version = vim.lsp.util.buf_versions[buffer]
  end
  client:request("gti/inspectMacroExpansion", params, function(err, result)
    if not vim.api.nvim_buf_is_valid(buffer) then return end
    for attached, generation in pairs(generations) do
      if not vim.api.nvim_buf_is_valid(attached)
        or vim.api.nvim_buf_get_changedtick(attached) ~= generation then
        vim.notify("Macro inspection discarded because source changed; request it again", vim.log.levels.INFO, { title = "GTI" })
        return
      end
    end
    if err then
      vim.notify(err.message or "Macro inspection failed", vim.log.levels.WARN, { title = "GTI" })
      return
    end
    if not result or result == vim.NIL then
      vim.notify("No unique checked macro expansion at the cursor", vim.log.levels.INFO, { title = "GTI" })
      return
    end
    if result.readOnly ~= true or type(result.text) ~= "string" then return end
    local scratch = vim.api.nvim_create_buf(false, true)
    vim.bo[scratch].buftype = "nofile"
    vim.bo[scratch].bufhidden = "wipe"
    vim.bo[scratch].swapfile = false
    vim.bo[scratch].filetype = "gti"
    vim.api.nvim_buf_set_lines(scratch, 0, -1, false, vim.split(result.text, "\n", { plain = true }))
    vim.bo[scratch].modifiable = false
    vim.bo[scratch].readonly = true
    vim.b[scratch].gti_macro_expansion = true
    vim.cmd("botright vnew")
    vim.api.nvim_win_set_buf(0, scratch)
    vim.keymap.set("n", "q", "<cmd>close<CR>", { buffer = scratch, silent = true })
  end, buffer)
end, { desc = "Inspect the checked macro expansion under the cursor (read-only)" })

vim.api.nvim_create_user_command("GTIInfo", function()
  local buffer = vim.api.nvim_get_current_buf()
  local name = vim.api.nvim_buf_get_name(buffer)
  local clients = vim.lsp.get_clients({ name = "gti_lsp", bufnr = buffer })
  local attached_root = clients[1] and clients[1].config and clients[1].config.root_dir
  local project_root = attached_root or vim.fs.root(buffer, { "gti.toml", ".git", "CMakeLists.txt", "gti-toolchain" })
  local info = toolchain.version_info({
    cwd = project_root or (name ~= "" and vim.fs.dirname(name) or nil),
  })
  local active_versions = {}
  local active_profiles = {}
  local problems = vim.deepcopy(info.problems)
  for _, client in ipairs(clients) do
    local version = client.server_info and client.server_info.version or "unknown"
    table.insert(active_versions, version)
    table.insert(active_profiles, requested_profile(client))
    if not info.managed and info.expected and version ~= info.expected then
      table.insert(problems, string.format(
        "running language server is %s but the plugin expects %s",
        version,
        info.expected
      ))
    end
  end

  local lines = {
    "Toolchain manager: " .. (info.managed and "gtiup" or "legacy plugin installer"),
    "Plugin version: " .. (info.expected or "unknown"),
    "Installed toolchain: " .. (info.installed or "not installed by Lazy"),
    string.format(
      "Compiler: %s (%s)",
      info.compiler,
      info.compiler_version or info.compiler_problem or "unknown"
    ),
    string.format(
      "Language server: %s (%s)",
      info.language_server,
      info.language_server_version or info.language_server_problem or "unknown"
    ),
    "Running LSP: " .. (#active_versions > 0 and table.concat(active_versions, ", ") or "not attached"),
    "Editor profile: " .. (#active_profiles > 0 and table.concat(active_profiles, ", ") or "project default"),
    "Tree-sitter parser: " .. (info.parser or "not found (using regex fallback)"),
    "Status: " .. (#problems == 0 and "versions agree" or table.concat(problems, "; ")),
  }
  local level = #problems == 0 and vim.log.levels.INFO or vim.log.levels.WARN
  vim.notify(table.concat(lines, "\n"), level, { title = "GTI" })
end, { desc = "Show the active GTI toolchain" })
