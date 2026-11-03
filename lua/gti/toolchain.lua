local M = {}

local function repository_root()
  local source = debug.getinfo(1, "S").source
  if source:sub(1, 1) ~= "@" then
    error("cannot locate the GTI plugin directory")
  end

  local path = vim.uv.fs_realpath(source:sub(2)) or source:sub(2)
  return vim.fs.dirname(vim.fs.dirname(vim.fs.dirname(path)))
end

local function is_executable(path)
  return path and path ~= "" and vim.fn.executable(path) == 1
end

local function is_file(path)
  local stat = path and path ~= "" and vim.uv.fs_stat(path) or nil
  return stat ~= nil and stat.type == "file"
end

local function read_version(path)
  if not is_file(path) then
    return nil
  end
  local lines = vim.fn.readfile(path, "", 1)
  if #lines == 0 then
    return nil
  end
  return vim.trim(lines[1])
end

local function executable_version(path)
  if not is_executable(path) then
    return nil, "not executable"
  end

  local result = vim.system({ path, "--version" }, { text = true }):wait(2000)
  if result.code ~= 0 then
    local details = vim.trim((result.stderr or "") .. "\n" .. (result.stdout or ""))
    return nil, details ~= "" and details or ("exit code " .. tostring(result.code))
  end

  local version = (result.stdout or ""):match("(%d+%.%d+%.%d+)")
  if not version then
    return nil, "did not report a semantic version"
  end
  return version, nil
end

local function parser_beside_executable(executable)
  if not executable or executable == "" then
    return nil
  end
  local real = vim.uv.fs_realpath(executable) or executable
  local prefix = vim.fs.dirname(vim.fs.dirname(real))
  local parser = vim.fs.joinpath(prefix, "share", "gti", "parser", "gti.so")
  return is_file(parser) and parser or nil
end

-- Managed mode delegates selection and installation to gtiup. It never falls
-- back to an unrelated plugin-local or PATH toolchain after selection fails.
function M.managed()
  return vim.g.gti_toolchain == "gtiup"
end

function M.managed_executable(name, opts)
  opts = opts or {}
  local manager = vim.env.GTIUP_PATH
  if not manager or manager == "" then manager = "gtiup" end
  local launched, result = pcall(function()
    return vim.system({ manager, "which", name }, {
      text = true,
      cwd = opts.cwd,
    }):wait(5000)
  end)
  if not launched then
    error("gtiup could not select " .. name .. ": cannot run " .. manager
      .. ". Install gtiup on Neovim's PATH or set GTIUP_PATH to its executable. " .. tostring(result))
  end
  local path = vim.trim(result.stdout or "")
  if result.code ~= 0 or not is_executable(path) then
    local detail = vim.trim(result.stderr or "")
    error("gtiup could not select " .. name .. ": "
      .. (detail ~= "" and detail or "install the project's selected toolchain with gtiup"))
  end
  return path
end

function M.root()
  return repository_root()
end

function M.bin_dir(root)
  return vim.fs.joinpath(root or M.root(), "toolchain", "bin")
end

function M.expected_version(root)
  return read_version(vim.fs.joinpath(root or M.root(), "VERSION"))
end

function M.installed_version(root)
  return read_version(vim.fs.joinpath(
    root or M.root(),
    "toolchain",
    "share",
    "gti",
    "VERSION"
  ))
end

function M.executable(name, opts)
  opts = opts or {}

  if name == "gti_lsp" and vim.env.GTI_LSP_PATH and vim.env.GTI_LSP_PATH ~= "" then
    return vim.env.GTI_LSP_PATH
  end
  if name == "gti" and vim.env.GTI_PATH and vim.env.GTI_PATH ~= "" then
    return vim.env.GTI_PATH
  end

  if M.managed() then
    return M.managed_executable(name, opts)
  end

  local root = opts.root or M.root()
  local bundled = vim.fs.joinpath(M.bin_dir(root), name)
  if is_executable(bundled) then
    return bundled
  end

  local from_path = vim.fn.exepath(name)
  if from_path ~= "" then
    return from_path
  end

  local development = vim.fs.joinpath(root, "build", name)
  if is_executable(development) then
    return development
  end

  return name
end

function M.parser(opts)
  opts = opts or {}
  if is_file(vim.env.GTI_TREE_SITTER_PATH) then
    return vim.env.GTI_TREE_SITTER_PATH
  end

  if M.managed() then
    local ok, executable = pcall(M.executable, "gti_lsp", opts)
    return ok and parser_beside_executable(executable) or nil
  end

  local root = opts.root or M.root()
  local bundled = vim.fs.joinpath(root, "toolchain", "share", "gti", "parser", "gti.so")
  if is_file(bundled) then
    return bundled
  end

  for _, name in ipairs({ "gti_lsp", "gti" }) do
    local executable = M.executable(name, { root = root })
    local installed = parser_beside_executable(vim.fn.exepath(executable) ~= "" and vim.fn.exepath(executable) or executable)
    if installed then
      return installed
    end
  end

  local development = vim.fs.joinpath(root, "build", "gti.so")
  if is_file(development) then
    return development
  end
  return nil
end

function M.version_info(opts)
  opts = opts or {}
  local root = opts.root or M.root()
  local expected = opts.expected_version or M.expected_version(root)
  local managed = M.managed()
  local installed = not managed and M.installed_version(root) or nil
  local resolve_options = { root = root, cwd = opts.cwd }
  local compiler_ok, compiler = pcall(M.executable, "gti", resolve_options)
  local server_ok, language_server = pcall(M.executable, "gti_lsp", resolve_options)
  local problems = {}
  if not compiler_ok then table.insert(problems, compiler); compiler = "unavailable" end
  if not server_ok then table.insert(problems, language_server); language_server = "unavailable" end
  if managed and server_ok then
    installed = read_version(vim.fs.joinpath(vim.fs.dirname(vim.fs.dirname(language_server)), "share", "gti", "VERSION"))
  end
  local compiler_version
  local compiler_problem
  local language_server_version
  local language_server_problem

  if opts.probe_binaries ~= false then
    if compiler_ok then compiler_version, compiler_problem = executable_version(compiler) end
    if server_ok then language_server_version, language_server_problem = executable_version(language_server) end
  end

  if not managed and expected and installed and expected ~= installed then
    table.insert(problems, string.format(
      "plugin expects %s but the installed toolchain is %s",
      expected,
      installed
    ))
  end
  for _, binary in ipairs({
    { label = "compiler", version = compiler_version, problem = compiler_problem },
    {
      label = "language server",
      version = language_server_version,
      problem = language_server_problem,
    },
  }) do
    local label = binary.label
    local version = binary.version
    if not managed and expected and version and expected ~= version then
      table.insert(problems, string.format(
        "%s is %s but the plugin expects %s",
        label,
        version,
        expected
      ))
    elseif opts.probe_binaries ~= false and not version then
      table.insert(problems, string.format(
        "%s version could not be verified: %s",
        label,
        binary.problem or "unknown error"
      ))
    end
  end

  if managed and compiler_version and language_server_version and compiler_version ~= language_server_version then
    table.insert(problems, "selected compiler and language server versions differ")
  end

  return {
    managed = managed,
    expected = expected,
    installed = installed,
    compiler = compiler,
    compiler_version = compiler_version,
    compiler_problem = compiler_problem,
    language_server = language_server,
    language_server_version = language_server_version,
    language_server_problem = language_server_problem,
    parser = M.parser(resolve_options),
    problems = problems,
  }
end

function M.prepend_path(root)
  if M.managed() then return false end
  local bin = M.bin_dir(root)
  if not is_executable(vim.fs.joinpath(bin, "gti")) then
    return false
  end

  local current = vim.env.PATH or ""
  local separator = package.config:sub(1, 1) == "\\" and ";" or ":"
  for entry in vim.gsplit(current, separator, { plain = true }) do
    if entry == bin then
      return true
    end
  end

  vim.env.PATH = current == "" and bin or (bin .. separator .. current)
  return true
end

return M
