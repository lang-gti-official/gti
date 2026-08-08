local M = {}

local repository = "LukeHHA/gti"

local function join(...)
  return vim.fs.joinpath(...)
end

local function exists(path)
  return vim.uv.fs_stat(path) ~= nil
end

local function read_file(path)
  local handle, open_error = vim.uv.fs_open(path, "r", 438)
  if not handle then
    error("could not open " .. path .. ": " .. tostring(open_error))
  end

  local stat, stat_error = vim.uv.fs_fstat(handle)
  if not stat then
    vim.uv.fs_close(handle)
    error("could not inspect " .. path .. ": " .. tostring(stat_error))
  end

  local data, read_error = vim.uv.fs_read(handle, stat.size, 0)
  vim.uv.fs_close(handle)
  if not data then
    error("could not read " .. path .. ": " .. tostring(read_error))
  end
  return data
end

local function trim(value)
  return value:match("^%s*(.-)%s*$")
end

local function run(command)
  local result = vim.system(command, { text = true }):wait()
  if result.code ~= 0 then
    local details = trim((result.stderr or "") .. "\n" .. (result.stdout or ""))
    return false, details ~= "" and details or ("exit code " .. tostring(result.code))
  end
  return true, result.stdout or ""
end

local function progress(opts, message)
  if opts.on_progress then
    opts.on_progress(message)
  end
end

local function detect_platform()
  local uname = vim.uv.os_uname()
  local system = uname.sysname
  local machine = uname.machine:lower()
  local architecture
  if machine == "x86_64" or machine == "amd64" then
    architecture = "x64"
  elseif machine == "arm64" or machine == "aarch64" then
    architecture = "arm64"
  end

  if system == "Darwin" and architecture then
    return "darwin-" .. architecture
  end
  if system == "Linux" and architecture then
    return "linux-" .. architecture
  end

  error(string.format(
    "GTI release binaries are not available for %s/%s; build GTI from source and set GTI_LSP_PATH",
    system,
    uname.machine
  ))
end

local function download(url, destination)
  local curl = vim.fn.exepath("curl")
  if curl ~= "" then
    local ok, message = run({
      curl,
      "-fL",
      "--retry",
      "3",
      "--connect-timeout",
      "15",
      "-o",
      destination,
      url,
    })
    if ok then
      return
    end
    if vim.fn.exepath("wget") == "" then
      error("curl could not download " .. url .. ": " .. message)
    end
  end

  local wget = vim.fn.exepath("wget")
  if wget == "" then
    error("install curl or wget so GTI can download its released toolchain")
  end
  local ok, message = run({ wget, "-O", destination, url })
  if not ok then
    error("wget could not download " .. url .. ": " .. message)
  end
end

local function verify_checksum(archive, checksum, expected_name)
  local checksum_text = read_file(checksum)
  local expected_hash, listed_name = checksum_text:match(
    "^%s*([0-9a-fA-F]+)%s+%*?([^\r\n]+)"
  )
  if not expected_hash or #expected_hash ~= 64 then
    error("invalid SHA-256 file: " .. checksum)
  end
  if trim(listed_name) ~= expected_name then
    error(string.format(
      "checksum names %s, but the expected archive is %s",
      trim(listed_name),
      expected_name
    ))
  end

  local actual_hash = vim.fn.sha256(read_file(archive))
  if actual_hash:lower() ~= expected_hash:lower() then
    error("SHA-256 verification failed for " .. archive)
  end
end

local function validate_archive_paths(archive)
  local ok, listing = run({ "tar", "-tzf", archive })
  if not ok then
    error("could not inspect GTI release archive: " .. listing)
  end

  for entry in listing:gmatch("[^\r\n]+") do
    local normalized = entry:gsub("^%./", "")
    if normalized:sub(1, 1) == "/" then
      error("release archive contains an absolute path: " .. entry)
    end
    for segment in normalized:gmatch("[^/]+") do
      if segment == ".." then
        error("release archive contains a parent-directory path: " .. entry)
      end
    end
  end
end

local function validate_toolchain(root, version)
  local required = {
    "bin/gti",
    "bin/gti_lsp",
    "lib/libgti_compiler.a",
    "lib/libgti_runtime.a",
    "share/gti/VERSION",
    "share/gti/parser/gti.so",
    "share/gti/stdlib/prelude.gti",
    "share/gti/stdlib/std/array.gti",
    "share/licenses/gti/GTI-LICENSE.txt",
    "share/licenses/gti/json-c-LICENSE.txt",
  }
  for _, relative in ipairs(required) do
    if not exists(join(root, relative)) then
      error("GTI toolchain is missing " .. relative)
    end
  end
  if vim.fn.executable(join(root, "bin", "gti")) ~= 1 then
    error("installed GTI compiler is not executable")
  end
  if vim.fn.executable(join(root, "bin", "gti_lsp")) ~= 1 then
    error("installed GTI language server is not executable")
  end
  if trim(read_file(join(root, "share", "gti", "VERSION"))) ~= version then
    error("installed GTI toolchain version does not match " .. version)
  end
end

local function current_install_matches(root, version)
  local version_path = join(root, "share", "gti", "VERSION")
  if not exists(version_path) then
    return false
  end
  if trim(read_file(version_path)) ~= version then
    return false
  end
  return vim.fn.executable(join(root, "bin", "gti")) == 1
    and vim.fn.executable(join(root, "bin", "gti_lsp")) == 1
    and exists(join(root, "share", "gti", "parser", "gti.so"))
    and exists(join(root, "share", "gti", "stdlib", "std", "array.gti"))
end

function M.platform()
  return detect_platform()
end

function M.install(opts)
  opts = opts or {}
  local root = assert(opts.root, "GTI installer requires the plugin root")
  local version = opts.version or trim(read_file(join(root, "VERSION")))
  local platform = opts.platform or detect_platform()
  local install_root = join(root, "toolchain")

  if not opts.force and current_install_matches(install_root, version) then
    progress(opts, "GTI " .. version .. " is already installed")
    return install_root
  end

  if vim.fn.exepath("tar") == "" then
    error("tar is required to install the GTI toolchain")
  end

  local archive_name = string.format("gti-v%s-%s.tar.gz", version, platform)
  local base_url = string.format(
    "https://github.com/%s/releases/download/v%s/%s",
    repository,
    version,
    archive_name
  )
  local temporary = join(root, string.format(
    ".gti-install-%d-%s",
    vim.fn.getpid(),
    tostring(vim.uv.hrtime())
  ))
  local extraction = join(temporary, "extract")
  local archive = opts.archive_path or join(temporary, archive_name)
  local checksum = opts.checksum_path or join(temporary, archive_name .. ".sha256")
  local backup = join(root, ".gti-toolchain-backup")

  vim.fn.mkdir(extraction, "p")
  local ok, result = xpcall(function()
    if not opts.archive_path then
      progress(opts, "Downloading " .. archive_name)
      download(base_url, archive)
    end
    if not opts.checksum_path then
      progress(opts, "Downloading " .. archive_name .. ".sha256")
      download(base_url .. ".sha256", checksum)
    end

    progress(opts, "Verifying " .. archive_name)
    verify_checksum(archive, checksum, archive_name)
    validate_archive_paths(archive)

    progress(opts, "Extracting GTI " .. version)
    local extracted, extract_error = run({ "tar", "-xzf", archive, "-C", extraction })
    if not extracted then
      error("could not extract GTI release archive: " .. extract_error)
    end
    validate_toolchain(extraction, version)

    if exists(backup) then
      vim.fn.delete(backup, "rf")
    end
    local had_previous = exists(install_root)
    if had_previous then
      local moved, move_error = vim.uv.fs_rename(install_root, backup)
      if not moved then
        error("could not preserve the previous GTI toolchain: " .. tostring(move_error))
      end
    end

    local installed, install_error = vim.uv.fs_rename(extraction, install_root)
    if not installed then
      if had_previous then
        vim.uv.fs_rename(backup, install_root)
      end
      error("could not activate the GTI toolchain: " .. tostring(install_error))
    end
    if had_previous then
      vim.fn.delete(backup, "rf")
    end

    progress(opts, "Installed GTI " .. version .. " for " .. platform)
    return install_root
  end, debug.traceback)

  vim.fn.delete(temporary, "rf")
  if not ok then
    error(result, 0)
  end
  return result
end

return M
