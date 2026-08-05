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

function M.root()
  return repository_root()
end

function M.bin_dir(root)
  return vim.fs.joinpath(root or M.root(), "toolchain", "bin")
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
