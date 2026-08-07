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
  if vim.fn.executable(compiler) ~= 1 or vim.fn.executable(language_server) ~= 1 then
    fail("installer did not produce executable tools")
  end
  if not vim.uv.fs_stat(tree_sitter_parser) then
    fail("installer did not produce the GTI Tree-sitter parser")
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
    "class Handle {",
    "  mut int value = 0;",
    "public:",
    "  int& operator*() { return self.value; }",
    "  mut int& operator*() mut { return self.value; }",
    "  bool operator==(nullptr_t other) { return false; }",
    "  operator bool() { return true; }",
    "};",
    "namespace std {",
    "uint64 pow(uint64 base, uint64 exponent) {",
    "mut uint64 result = 1;",
    "auto multiply = [base](uint64 value) -> uint64 { return base * value; };",
    "for (mut uint64 i = 0; i < exponent; i++) { result = result * base; }",
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
  for _, query_name in ipairs({ "highlights", "indents", "folds" }) do
    if not vim.treesitter.query.get("gti", query_name) then
      fail("GTI Tree-sitter " .. query_name .. " query did not load")
    end
  end
  local highlight_query = vim.treesitter.query.get("gti", "highlights")
  local captures = {}
  for capture_id in highlight_query:iter_captures(root_node, 0, 0, -1) do
    captures[highlight_query.captures[capture_id]] = true
  end
  for _, capture in ipairs({ "function", "keyword", "keyword.operator", "operator", "type.builtin", "variable", "variable.parameter" }) do
    if not captures[capture] then
      fail("GTI Tree-sitter highlighting did not capture " .. capture)
    end
  end

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

  local unformatted = table.concat(vim.api.nvim_buf_get_lines(0, 0, -1, false), "\n")
  vim.lsp.buf.format({ async = false, timeout_ms = 5000 })
  if table.concat(vim.api.nvim_buf_get_lines(0, 0, -1, false), "\n") == unformatted then
    fail("gti_lsp did not format the GTI buffer")
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
