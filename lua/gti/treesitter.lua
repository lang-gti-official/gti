local M = {}

local configured = false
local configured_parser
local managed_registered = false
local reported_parsers = {}

local function start_buffer(buffer)
  local ok = pcall(vim.treesitter.start, buffer, "gti")
  if not ok then
    return
  end

  local has_nvim_treesitter, nvim_treesitter = pcall(require, "nvim-treesitter")
  if has_nvim_treesitter and type(nvim_treesitter.indentexpr) == "function" then
    vim.bo[buffer].indentexpr = 'v:lua.require"nvim-treesitter".indentexpr()'
  end
end

function M.setup(toolchain, opts)
  if toolchain.managed() and not opts then
    if not managed_registered then
      managed_registered = true
      vim.api.nvim_create_autocmd("FileType", {
        group = vim.api.nvim_create_augroup("gti_treesitter_managed", { clear = true }),
        pattern = "gti",
        callback = function(args)
          local name = vim.api.nvim_buf_get_name(args.buf)
          if M.setup(toolchain, { cwd = name ~= "" and vim.fs.dirname(name) or nil }) then
            start_buffer(args.buf)
          else
            pcall(vim.treesitter.stop, args.buf, "gti")
          end
        end,
      })
      for _, buffer in ipairs(vim.api.nvim_list_bufs()) do
        if vim.api.nvim_buf_is_loaded(buffer) and vim.bo[buffer].filetype == "gti" then
          local name = vim.api.nvim_buf_get_name(buffer)
          if M.setup(toolchain, { cwd = name ~= "" and vim.fs.dirname(name) or nil }) then
            start_buffer(buffer)
          end
        end
      end
    end
    return true
  end
  local parser = toolchain.parser(opts)
  if configured_parser then
    if parser == configured_parser then return configured end
    -- Neovim's grammar is process-global; never silently parse another
    -- toolchain's source using a different grammar already loaded in this UI.
    local key = parser or "unavailable"
    if not reported_parsers[key] then
      reported_parsers[key] = true
      vim.notify("GTI project selects a different or unavailable Tree-sitter parser; "
        .. "using regex syntax for this buffer. Open the project in a separate Neovim instance.", vim.log.levels.WARN)
    end
    return false
  end
  if not parser then
    return false
  end

  local loaded, problem = pcall(vim.treesitter.language.add, "gti", { path = parser })
  if not loaded then
    vim.schedule(function()
      vim.notify("Could not load the GTI Tree-sitter parser: " .. tostring(problem), vim.log.levels.WARN)
    end)
    return false
  end

  configured_parser = parser

  -- The parser comes from the released toolchain while the queries come from
  -- this checkout, so an update can leave the two disagreeing until Neovim
  -- restarts. Parse the queries against the parser we just registered instead
  -- of letting the first renderer discover the mismatch: `query.get` is
  -- memoized, so an unchecked stale pair surfaces as an opaque node-type error
  -- inside whichever plugin draws a GTI buffer first.
  local usable, query_problem = pcall(vim.treesitter.query.get, "gti", "highlights")
  if not usable then
    local expected = toolchain.expected_version() or "unknown"
    local installed = toolchain.installed_version() or "unknown"
    vim.schedule(function()
      vim.notify(
        "GTI Tree-sitter queries ("
          .. expected
          .. ") do not match the installed parser ("
          .. installed
          .. "); falling back to regex syntax. Update the toolchain, then"
          .. " restart Neovim. "
          .. tostring(query_problem),
        vim.log.levels.WARN
      )
    end)
    return false
  end

  configured = true
  configured_parser = parser
  if toolchain.managed() then return true end
  vim.api.nvim_create_autocmd("FileType", {
    group = vim.api.nvim_create_augroup("gti_treesitter", { clear = true }),
    pattern = "gti",
    callback = function(args)
      start_buffer(args.buf)
    end,
  })

  for _, buffer in ipairs(vim.api.nvim_list_bufs()) do
    if vim.api.nvim_buf_is_loaded(buffer) and vim.bo[buffer].filetype == "gti" then
      start_buffer(buffer)
    end
  end
  return true
end

return M
