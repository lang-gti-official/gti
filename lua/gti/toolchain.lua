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
  local installed = M.installed_version(root)
  local compiler = M.executable("gti", { root = root })
  local language_server = M.executable("gti_lsp", { root = root })
  local compiler_version
  local compiler_problem
  local language_server_version
  local language_server_problem

  if opts.probe_binaries ~= false then
    compiler_version, compiler_problem = executable_version(compiler)
    language_server_version, language_server_problem = executable_version(language_server)
  end

  local problems = {}
  if expected and installed and expected ~= installed then
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
    if expected and version and expected ~= version then
      table.insert(problems, string.format(
        "%s is %s but the plugin expects %s",
        label,
        version,
        expected
      ))
    elseif expected and opts.probe_binaries ~= false and not version then
      table.insert(problems, string.format(
        "%s version could not be verified: %s",
        label,
        binary.problem or "unknown error"
      ))
    end
  end

  return {
    expected = expected,
    installed = installed,
    compiler = compiler,
    compiler_version = compiler_version,
    compiler_problem = compiler_problem,
    language_server = language_server,
    language_server_version = language_server_version,
    language_server_problem = language_server_problem,
    parser = M.parser({ root = root }),
    problems = problems,
  }
end

function M.prepend_path(root)
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
