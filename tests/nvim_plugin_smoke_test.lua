local repository = assert(vim.env.GTI_TEST_REPOSITORY, "GTI_TEST_REPOSITORY is required")
local archive = assert(vim.env.GTI_TEST_ARCHIVE, "GTI_TEST_ARCHIVE is required")
local checksum = assert(vim.env.GTI_TEST_CHECKSUM, "GTI_TEST_CHECKSUM is required")
local platform = assert(vim.env.GTI_TEST_PLATFORM, "GTI_TEST_PLATFORM is required")
local version = vim.trim(table.concat(vim.fn.readfile(vim.fs.joinpath(repository, "VERSION")), "\n"))

local function fail(message)
  error("Neovim plugin smoke test: " .. message)
end

local temporary = vim.fn.tempname()
vim.fn.mkdir(temporary, "p")

local ok, problem = xpcall(function()
  local lazy_spec = dofile(vim.fs.joinpath(repository, "lazy.lua"))
  if lazy_spec[1] ~= "LukeHHA/gti"
    or lazy_spec.lazy ~= false
    or lazy_spec.build ~= "build.lua"
  then
    fail("lazy.lua does not expose the self-installing GTI plugin spec")
  end

  local installer = dofile(vim.fs.joinpath(repository, "lua", "gti", "installer.lua"))
  local installed = installer.install({
    root = temporary,
    version = version,
    platform = platform,
    archive_path = archive,
    checksum_path = checksum,
  })
  local reused = installer.install({
    root = temporary,
    version = version,
    platform = platform,
  })
  if reused ~= installed then
    fail("installer did not reuse the matching installed toolchain")
  end

  local compiler = vim.fs.joinpath(installed, "bin", "gti")
  local language_server = vim.fs.joinpath(installed, "bin", "gti_lsp")
  local tree_sitter_parser = vim.fs.joinpath(installed, "share", "gti", "parser", "gti.so")
  local string_unit = vim.fs.joinpath(installed, "share", "gti", "stdlib", "std", "string.gti")
  if vim.fn.executable(compiler) ~= 1 or vim.fn.executable(language_server) ~= 1 then
    fail("installer did not produce executable tools")
  end
  if not vim.uv.fs_stat(tree_sitter_parser) then
    fail("installer did not produce the GTI Tree-sitter parser")
  end
  if not vim.uv.fs_stat(string_unit) then
    fail("installer did not include std::string")
  end

  vim.opt.runtimepath:prepend(repository)
  package.path = vim.fs.joinpath(repository, "lua", "?.lua")
    .. ";"
    .. vim.fs.joinpath(repository, "lua", "?", "init.lua")
    .. ";"
    .. package.path
  vim.env.GTI_PATH = compiler
  vim.env.GTI_LSP_PATH = language_server
  vim.env.GTI_TREE_SITTER_PATH = tree_sitter_parser

  vim.cmd("filetype plugin on")
  vim.cmd("syntax on")
  vim.cmd("runtime plugin/gti.lua")

  local project = vim.fs.joinpath(temporary, "project")
  vim.fn.mkdir(vim.fs.joinpath(project, ".git"), "p")
  local source_path = vim.fs.joinpath(project, "smoke.gti")
  vim.fn.writefile({
    "include <std/array>",
    "include <std/string>",
    "using Index = uint64_t;",
    "enum class Stage : uint8_t { Boot, Running = 4, };",
    "class StaticArray<T, uint64_t N> {",
    "  T values[N] = {};",
    "public:",
    "  uint64_t size() { return N; }",
    "};",
    "interface Renderable {",
    "  int render(int frame) = 0;",
    "};",
    "class Renderer : public Renderable {",
    "public:",
    "  int render(int frame) override { return frame; }",
    "};",
    "class Handle {",
    "  mut int value = 0;",
    "public:",
    "  int& operator*() { return this.value; }",
    "  mut int& operator*() mut { return this.value; }",
    "  int operator()(int offset) { int self = 1; return this.value + offset + self; }",
    "  bool operator==(nullptr_t other) { return false; }",
    "  operator bool() { return true; }",
    "};",
    "T constrained<std::ordered T>(T value) { return value; }",
    "StaticArray<int, 4> direct_array{};",
    "T choose<std::ordered T>(T left, T right) {",
    "  if (left > right) {",
    "    return left;",
    "  }",
    "  return right;",
    "}",
    "char marker = 'G';",
    "namespace std {",
    "uint64_t pow(uint64_t base, uint64_t exponent) {",
    "mut uint64_t result = 1;",
    "switch (exponent) {",
    "case uint64_t(0):",
    "  return uint64_t(1);",
    "default:",
    "  break;",
    "}",
    'mut std::string label = std::string("gti");',
    "label.push_back('!');",
    "auto multiply = [base](uint64_t value) -> uint64_t { return base * value; };",
    "for (mut uint64_t i = 0; i < exponent; i++) { result = result * base; }",
    "return multiply(result);",
    "}",
    "}",
  }, source_path)
  vim.cmd("edit " .. vim.fn.fnameescape(source_path))

  if vim.bo.filetype ~= "gti" then
    fail("*.gti did not select the gti filetype")
  end
  if vim.bo.syntax ~= "" and vim.bo.syntax ~= "gti" then
    fail("GTI selected an unexpected fallback syntax")
  end
  if vim.bo.commentstring ~= "// %s" then
    fail("GTI filetype settings did not load")
  end
  local parser = vim.treesitter.get_parser(0, "gti", {})
  local root_node = parser:parse()[1]:root()
  if root_node:has_error() then
    fail("GTI Tree-sitter parser produced an error node for valid source")
  end
  if not vim.treesitter.highlighter.active[vim.api.nvim_get_current_buf()] then
    fail("GTI Tree-sitter highlighting did not start")
  end
  for _, query_name in ipairs({ "highlights", "indents", "folds", "locals" }) do
    if not vim.treesitter.query.get("gti", query_name) then
      fail("GTI Tree-sitter " .. query_name .. " query did not load")
    end
  end
  local highlight_query = vim.treesitter.query.get("gti", "highlights")
  local captures = {}
  local captures_by_position = {}
  for capture_id, node in highlight_query:iter_captures(root_node, 0, 0, -1) do
    local capture = highlight_query.captures[capture_id]
    captures[capture] = true
    local row, column = node:start()
    local key = row .. ":" .. column
    captures_by_position[key] = captures_by_position[key] or {}
    captures_by_position[key][capture] = true
  end
  for _, capture in ipairs({ "character", "constant", "function", "keyword.conditional", "keyword.operator", "keyword.repeat", "keyword.return", "module", "operator", "punctuation.bracket", "punctuation.delimiter", "type", "type.builtin", "type.parameter", "variable", "variable.parameter" }) do
    if not captures[capture] then
      fail("GTI Tree-sitter highlighting did not capture " .. capture)
    end
  end

  local source_lines = vim.api.nvim_buf_get_lines(0, 0, -1, false)
  local function require_capture(line_text, token, capture)
    for row, line in ipairs(source_lines) do
      if line == line_text then
        local start = line:find(token, 1, true)
        if not start then
          fail("could not locate '" .. token .. "' in Tree-sitter fixture line")
        end
        local at_position = captures_by_position[(row - 1) .. ":" .. (start - 1)] or {}
        if not at_position[capture] then
          fail("GTI Tree-sitter did not capture '" .. token .. "' as " .. capture)
        end
        return
      end
    end
    fail("could not locate Tree-sitter fixture line: " .. line_text)
  end

  require_capture("T choose<std::ordered T>(T left, T right) {", "left", "variable.parameter")
  require_capture("  if (left > right) {", "if", "keyword.conditional")
  require_capture("  if (left > right) {", "left", "variable")
  require_capture("    return left;", "return", "keyword.return")
  require_capture("switch (exponent) {", "switch", "keyword.conditional")
  require_capture("case uint64_t(0):", "case", "keyword.conditional")
  require_capture("default:", "default", "keyword.conditional")
  require_capture("StaticArray<int, 4> direct_array{};", "StaticArray", "type")
  require_capture("StaticArray<int, 4> direct_array{};", "direct_array", "variable")
  require_capture("interface Renderable {", "interface", "keyword.type")
  require_capture("interface Renderable {", "Renderable", "type.definition")
  require_capture("class Renderer : public Renderable {", "Renderable", "type")
  require_capture("  int render(int frame) override { return frame; }", "override", "keyword.modifier")
  require_capture("for (mut uint64_t i = 0; i < exponent; i++) { result = result * base; }", "for", "keyword.repeat")
  require_capture("enum class Stage : uint8_t { Boot, Running = 4, };", "Boot", "constant")
  require_capture("  int operator()(int offset) { int self = 1; return this.value + offset + self; }", "this", "variable.builtin")
  require_capture("  int operator()(int offset) { int self = 1; return this.value + offset + self; }", "self", "variable")

  local locals_query = vim.treesitter.query.get("gti", "locals")
  local local_captures_by_position = {}
  for capture_id, node in locals_query:iter_captures(root_node, 0, 0, -1) do
    local capture = locals_query.captures[capture_id]
    local row, column = node:start()
    local key = row .. ":" .. column
    local_captures_by_position[key] = local_captures_by_position[key] or {}
    local_captures_by_position[key][capture] = true
  end

  local function require_local_capture(line_text, token, capture)
    for row, line in ipairs(source_lines) do
      if line == line_text then
        local start = line:find(token, 1, true)
        if not start then
          fail("could not locate '" .. token .. "' in locals fixture line")
        end
        local at_position = local_captures_by_position[(row - 1) .. ":" .. (start - 1)] or {}
        if not at_position[capture] then
          fail("GTI Tree-sitter locals did not capture '" .. token .. "' as " .. capture)
        end
        return
      end
    end
    fail("could not locate locals fixture line: " .. line_text)
  end

  require_local_capture("T choose<std::ordered T>(T left, T right) {", "left", "local.definition.parameter")
  require_local_capture("  if (left > right) {", "left", "local.reference")

  local type_parameter_hl = vim.api.nvim_get_hl(0, {
    name = "@lsp.type.typeParameter.gti",
    link = true,
  })
  if type_parameter_hl.link ~= "@type.definition" then
    fail("GTI semantic type-parameter highlighting was not linked")
  end
  local property_hl = vim.api.nvim_get_hl(0, {
    name = "@lsp.type.property.gti",
    link = true,
  })
  if property_hl.link ~= "@variable.member" then
    fail("GTI semantic property highlighting was not linked")
  end
  local enum_member_hl = vim.api.nvim_get_hl(0, {
    name = "@lsp.type.enumMember.gti",
    link = true,
  })
  if enum_member_hl.link ~= "@constant" then
    fail("GTI semantic enum-member highlighting was not linked")
  end
  local function_scope_hl = vim.api.nvim_get_hl(0, {
    name = "@lsp.typemod.parameter.functionScope.gti",
    link = true,
  })
  if function_scope_hl.link ~= "@variable.parameter" then
    fail("GTI semantic function-scope parameters were not linked")
  end

  local attached = vim.wait(10000, function()
    return #vim.lsp.get_clients({ bufnr = 0, name = "gti_lsp" }) == 1
  end, 25)
  if not attached then
    fail("gti_lsp did not attach to a GTI buffer")
  end
  local client = vim.lsp.get_clients({ bufnr = 0, name = "gti_lsp" })[1]
  if not client.server_capabilities.semanticTokensProvider then
    fail("gti_lsp did not advertise semantic tokens alongside Tree-sitter")
  end
  if not client.server_capabilities.hoverProvider then
    fail("gti_lsp did not advertise semantic hover")
  end
  if not client.server_capabilities.completionProvider then
    fail("gti_lsp did not advertise semantic completion")
  end
  if not client.server_info or client.server_info.version ~= version then
    fail("gti_lsp did not report the installed toolchain version")
  end

  local unformatted = table.concat(vim.api.nvim_buf_get_lines(0, 0, -1, false), "\n")
  vim.lsp.buf.format({ async = false, timeout_ms = 5000 })
  if table.concat(vim.api.nvim_buf_get_lines(0, 0, -1, false), "\n") == unformatted then
    fail("gti_lsp did not format the GTI buffer")
  end
  local formatted = table.concat(vim.api.nvim_buf_get_lines(0, 0, -1, false), "\n")
  if not formatted:find("include <std/array>", 1, true)
    or not formatted:find("include <std/string>", 1, true)
    or not formatted:find("class StaticArray<T, uint64_t N>", 1, true)
    or not formatted:find("interface Renderable {", 1, true)
    or not formatted:find("class Renderer : public Renderable {", 1, true)
    or not formatted:find("int render(int frame) override {", 1, true)
    or not formatted:find("T constrained<std::ordered T>(T value)", 1, true)
    or not formatted:find("StaticArray<int, 4> direct_array{};", 1, true)
    or not formatted:find("char marker = 'G';", 1, true)
  then
    fail("gti_lsp formatting regressed imports or generic parameters")
  end

  local toolchain = require("gti.toolchain")
  local configured_compiler = vim.env.GTI_PATH
  local configured_parser = vim.env.GTI_TREE_SITTER_PATH
  vim.env.GTI_PATH = nil
  vim.env.GTI_TREE_SITTER_PATH = nil
  if toolchain.executable("gti", { root = temporary }) ~= compiler then
    fail("plugin did not resolve the installed compiler")
  end
  if toolchain.parser({ root = temporary }) ~= tree_sitter_parser then
    fail("plugin did not resolve the installed Tree-sitter parser")
  end
  local version_info = toolchain.version_info({
    root = temporary,
    expected_version = version,
  })
  if version_info.installed ~= version
    or version_info.compiler_version ~= version
    or version_info.language_server_version ~= version
    or #version_info.problems ~= 0
  then
    fail("plugin did not report matching compiler and language-server versions")
  end
  local mismatch = toolchain.version_info({
    root = temporary,
    expected_version = "9.9.9",
  })
  if #mismatch.problems < 2 then
    fail("plugin did not identify stale compiler and language-server versions")
  end
  vim.env.GTI_PATH = configured_compiler
  vim.env.GTI_TREE_SITTER_PATH = configured_parser
end, debug.traceback)

for _, client in ipairs(vim.lsp.get_clients()) do
  client:stop(true)
end
vim.fn.delete(temporary, "rf")

if not ok then
  error(problem, 0)
end
