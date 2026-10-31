local M = {}

-- Toolchain archives publish to GitHub Releases. Git tags created before the
-- switch keep their own installer, which downloads from the frozen Gitea host.
local release_server = "https://github.com"
local repository = "lang-gti-official/gti"

local function release_url(tag, asset)
  return string.format(
    "%s/%s/releases/download/%s/%s",
    release_server,
    repository,
    tag,
    asset
  )
end

M.release_url = release_url

-- The release train (scripts/release_train.py) keeps the yank list, withdrawn
-- betas and the published stable versions in one JSON document on the
-- `release-status` branch. A Git branch rather than a release asset: GitHub's
-- release immutability locks every published release's assets.
local release_status_url = string.format(
  "https://raw.githubusercontent.com/%s/release-status/release-status.json",
  repository
)

M.release_status_url = release_status_url

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
    if architecture == "x64" then
      -- Intel macOS artifacts stopped publishing after v0.299.0; the last
      -- Intel-capable release remains installable by pinning that version.
      error(
        "gti: darwin-x64 builds are no longer published; the last release "
          .. "with an Intel macOS archive is v0.299.0"
      )
    end
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

-- A release is identified by one file inside the toolchain. Stable releases
-- use VERSION, which changes per release. The nightly channel republishes one
-- rolling release whose VERSION does not move between builds, so it is
-- identified by BUILD_ID -- the commit it was built from -- instead.
local function validate_toolchain(root, identity)
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
    identity.relative,
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
  if trim(read_file(join(root, identity.relative))) ~= identity.value then
    error("installed GTI toolchain does not match " .. identity.label)
  end
end

local function current_install_matches(root, identity)
  local identity_path = join(root, identity.relative)
  if not exists(identity_path) then
    return false
  end
  if trim(read_file(identity_path)) ~= identity.value then
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

-- The nightly BUILD_ID is published as its own small asset so an installed
-- toolchain can decide whether it is current without downloading a full
-- platform archive on every sync.
local function fetch_nightly_build_id(root)
  local temporary = join(root, string.format(
    ".gti-build-id-%d-%s",
    vim.fn.getpid(),
    tostring(vim.uv.hrtime())
  ))
  vim.fn.mkdir(temporary, "p")
  local path = join(temporary, "BUILD_ID")
  local ok, failure = pcall(download, release_url("nightly", "BUILD_ID"), path)
  local build_id = ok and trim(read_file(path)) or nil
  vim.fn.delete(temporary, "rf")
  if not ok then
    error("could not read the nightly build identity: " .. tostring(failure))
  end
  if not build_id or not build_id:match("^%x+$") then
    error("the nightly build identity is not a commit identity")
  end
  return build_id
end

local function resolve_release(opts, root, platform)
  local channel = opts.channel or "stable"
  if channel == "nightly" then
    local build_id = opts.build_id or fetch_nightly_build_id(root)
    return {
      channel = "nightly",
      tag = "nightly",
      archive_name = string.format("gti-nightly-%s.tar.gz", platform),
      identity = {
        relative = "share/gti/BUILD_ID",
        value = build_id,
        label = "nightly " .. build_id:sub(1, 8),
      },
    }
  end
  if channel ~= "stable" then
    error("unknown GTI release channel: " .. tostring(channel))
  end
  local version = opts.version or trim(read_file(join(root, "VERSION")))
  return {
    channel = "stable",
    tag = "v" .. version,
    archive_name = string.format("gti-v%s-%s.tar.gz", version, platform),
    identity = {
      relative = "share/gti/VERSION",
      value = version,
      label = "GTI " .. version,
    },
  }
end

-- A checkout can name a VERSION whose stable release does not exist: main and
-- beta tags run ahead of the newest stable, which the release train promotes
-- only after a beta soaks. Say so, and name the nightly channel, rather than
-- installing some other version.
local function stable_unavailable(release, failure)
  return string.format(
    "gti: no stable GTI %s toolchain could be downloaded from GitHub Releases "
      .. "(%s). Stable archives exist only for published v<VERSION> releases; "
      .. "until %s is published, set vim.g.gti_channel = \"nightly\" (or "
      .. "GTI_CHANNEL=nightly) and run :Lazy build gti, or build GTI locally "
      .. "and set GTI_PATH and GTI_LSP_PATH. Cause: %s",
    release.tag,
    release_url(release.tag, release.archive_name),
    release.tag,
    tostring(failure)
  )
end

M.stable_unavailable = stable_unavailable

local function version_parts(value)
  local major, minor, patch = tostring(value):match("^(%d+)%.(%d+)%.(%d+)$")
  if not major then
    return nil
  end
  return { tonumber(major), tonumber(minor), tonumber(patch) }
end

local function version_less(left, right)
  local a, b = version_parts(left), version_parts(right)
  for index = 1, 3 do
    if a[index] ~= b[index] then
      return a[index] < b[index]
    end
  end
  return false
end

local function parse_release_status(text)
  local ok, status = pcall(vim.json.decode, text)
  if not ok or type(status) ~= "table" or status.schema ~= 1
    or type(status.stable) ~= "table" or type(status.yanked) ~= "table"
  then
    error("the GTI release status is not a schema 1 document")
  end
  for _, version in ipairs(status.stable) do
    if not version_parts(version) then
      error("the GTI release status lists an invalid stable version")
    end
  end
  for _, entry in ipairs(status.yanked) do
    if type(entry) ~= "table" or not version_parts(entry.version) or type(entry.reason) ~= "string" then
      error("the GTI release status has a malformed yank entry")
    end
  end
  return status
end

M.parse_release_status = parse_release_status

local function fetch_release_status(opts, root)
  if opts.release_status_path then
    return parse_release_status(read_file(opts.release_status_path))
  end
  local temporary = join(root, string.format(
    ".gti-release-status-%d-%s",
    vim.fn.getpid(),
    tostring(vim.uv.hrtime())
  ))
  vim.fn.mkdir(temporary, "p")
  local path = join(temporary, "release-status.json")
  local ok, result = pcall(function()
    download(release_status_url, path)
    return parse_release_status(read_file(path))
  end)
  vim.fn.delete(temporary, "rf")
  if not ok then
    error(result, 0)
  end
  return result
end

-- The yank entry for version, and the newest stable version that is not
-- yanked (nil when none is).
local function yank_of(status, version)
  local yanked, entry = {}, nil
  for _, item in ipairs(status.yanked) do
    yanked[item.version] = true
    if item.version == version then
      entry = item
    end
  end
  local newest
  for _, candidate in ipairs(status.stable) do
    if not yanked[candidate] and (not newest or version_less(newest, candidate)) then
      newest = candidate
    end
  end
  return entry, newest
end

local function yanked_message(version, entry, newest, installed)
  local next_step
  if newest then
    next_step = string.format(
      "The newest stable release that is not yanked is v%s: update the plugin "
        .. "(:Lazy update gti) or pin it in your Lazy spec (version = \"%s\"), "
        .. "then run :Lazy build gti",
      newest,
      newest
    )
  else
    next_step = "No stable release is currently installable"
  end
  return string.format(
    "gti: GTI %s was yanked (%s). %s. %s; or set vim.g.gti_channel = \"nightly\" "
      .. "(or GTI_CHANNEL=nightly). The installer never substitutes another version.",
    version,
    entry.reason,
    installed and "The installed toolchain is that yanked version"
      or "The installer does not install a yanked toolchain",
    next_step
  )
end

-- A stable install consults the release status first: a yanked version is
-- refused with the newest non-yanked stable named, never replaced silently.
local function check_not_yanked(opts, root, release, installed)
  local ok, status = pcall(fetch_release_status, opts, root)
  if not ok then
    if installed then
      progress(opts, "Could not read the GTI release status; keeping the installed " .. release.identity.label)
      return
    end
    error(string.format(
      "gti: could not read the GTI release status from %s to check that %s is not "
        .. "yanked, so nothing was installed. Retry when GitHub is reachable, or set "
        .. "vim.g.gti_channel = \"nightly\". Cause: %s",
      release_status_url,
      release.tag,
      tostring(status)
    ), 0)
  end
  local entry, newest = yank_of(status, release.identity.value)
  if entry then
    error(yanked_message(release.identity.value, entry, newest, installed), 0)
  end
end

function M.install(opts)
  opts = opts or {}
  local root = assert(opts.root, "GTI installer requires the plugin root")
  local platform = opts.platform or detect_platform()
  local install_root = join(root, "toolchain")
  local release = resolve_release(opts, root, platform)
  local identity = release.identity
  -- A locally built archive (scripts/install_neovim_local.py) is not a
  -- published release and is installed offline.
  local published = release.channel == "stable" and not opts.archive_path

  if not opts.force and current_install_matches(install_root, identity) then
    if published then
      check_not_yanked(opts, root, release, true)
    end
    progress(opts, identity.label .. " is already installed")
    return install_root
  end

  if published then
    check_not_yanked(opts, root, release, false)
  end

  if vim.fn.exepath("tar") == "" then
    error("tar is required to install the GTI toolchain")
  end

  local archive_name = release.archive_name
  local base_url = release_url(release.tag, archive_name)
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
      local downloaded, failure = pcall(download, base_url, archive)
      if not downloaded then
        if release.channel == "stable" then
          error(stable_unavailable(release, failure), 0)
        end
        error(failure, 0)
      end
    end
    if not opts.checksum_path then
      progress(opts, "Downloading " .. archive_name .. ".sha256")
      download(base_url .. ".sha256", checksum)
    end

    progress(opts, "Verifying " .. archive_name)
    verify_checksum(archive, checksum, archive_name)
    validate_archive_paths(archive)

    progress(opts, "Extracting " .. identity.label)
    local extracted, extract_error = run({ "tar", "-xzf", archive, "-C", extraction })
    if not extracted then
      error("could not extract GTI release archive: " .. extract_error)
    end
    validate_toolchain(extraction, identity)

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

    progress(opts, "Installed " .. identity.label .. " for " .. platform)
    return install_root
  end, debug.traceback)

  vim.fn.delete(temporary, "rf")
  if not ok then
    error(result, 0)
  end
  return result
end

return M
