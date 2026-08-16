local M = {}

local configured = false

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

function M.setup(toolchain)
  if configured then
    return true
  end

  local parser = toolchain.parser()
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
