local source = debug.getinfo(1, "S").source
if source:sub(1, 1) ~= "@" then
  error("highlight-inspector cannot locate its script directory")
end

local script_path = vim.uv.fs_realpath(source:sub(2)) or source:sub(2)
local tool_dir = vim.fs.dirname(script_path)
local repository_root = vim.fs.dirname(vim.fs.dirname(tool_dir))
package.path = tool_dir .. "/?.lua;" .. package.path

local compare = require("compare")

local function fail(message)
  error(message, 0)
end

local function usage()
  return [[
Usage: scripts/highlight-inspector/run [options]

Compare Tree-sitter captures, selected LSP semantic tokens, and resolved
github-theme highlights for equivalent C++ and GTI fixture probes.

Options:
  --output PATH                 Output directory (default: tool/output)
  --cpp-client NAME            Expected C++ client name (default: clangd)
  --gti-client NAME            Expected GTI client name (default: gti_lsp)
  --cpp-command PATH           C++ language-server executable
  --gti-command PATH           GTI language-server executable
  --cpp-parser PATH            C++ Tree-sitter parser shared library
  --gti-parser PATH            GTI Tree-sitter parser shared library
  --gti-runtime PATH           Installed GTI Neovim runtime root
  --cpp-fixture PATH           Alternate C++ fixture
  --gti-fixture PATH           Alternate GTI fixture
  --timeout MS                 Per-requirement timeout (default: 15000)
  --fail-on-visual-difference  Exit nonzero for final visual mismatches
  -h, --help                   Show this help

Client NAME may also be an executable path. Explicit *-command options take
precedence over executable discovery.
]]
end

local function take_value(arguments, index, option)
  local value = arguments[index + 1]
  if not value or value:sub(1, 2) == "--" then
    fail(option .. " requires a value")
  end
  return value, index + 2
end

local function parse_options(arguments)
  local options = {
    output = vim.fs.joinpath(tool_dir, "output"),
    cpp_client = "clangd",
    gti_client = "gti_lsp",
    cpp_fixture = vim.fs.joinpath(tool_dir, "fixtures", "comparison.cpp"),
    gti_fixture = vim.fs.joinpath(tool_dir, "fixtures", "comparison.gti"),
    timeout = 15000,
    fail_on_visual_difference = false,
  }

  local index = 1
  while index <= #arguments do
    local option = arguments[index]
    if option == "-h" or option == "--help" then
      options.help = true
      index = index + 1
    elseif option == "--fail-on-visual-difference" then
      options.fail_on_visual_difference = true
      index = index + 1
    elseif option == "--output" then
      options.output, index = take_value(arguments, index, option)
    elseif option == "--cpp-client" then
      options.cpp_client, index = take_value(arguments, index, option)
    elseif option == "--gti-client" then
      options.gti_client, index = take_value(arguments, index, option)
    elseif option == "--cpp-command" then
      options.cpp_command, index = take_value(arguments, index, option)
    elseif option == "--gti-command" then
      options.gti_command, index = take_value(arguments, index, option)
    elseif option == "--cpp-parser" then
      options.cpp_parser, index = take_value(arguments, index, option)
    elseif option == "--gti-parser" then
      options.gti_parser, index = take_value(arguments, index, option)
    elseif option == "--gti-runtime" then
      options.gti_runtime, index = take_value(arguments, index, option)
    elseif option == "--cpp-fixture" then
      options.cpp_fixture, index = take_value(arguments, index, option)
    elseif option == "--gti-fixture" then
      options.gti_fixture, index = take_value(arguments, index, option)
    elseif option == "--timeout" then
      local value
      value, index = take_value(arguments, index, option)
      options.timeout = tonumber(value)
      if not options.timeout or options.timeout < 100 then
        fail("--timeout must be a number of milliseconds at least 100")
      end
    else
      fail("unknown option: " .. option .. "\n\n" .. usage())
    end
  end
  return options
end

local function is_file(path)
  local stat = path and vim.uv.fs_stat(path) or nil
  return stat ~= nil and stat.type == "file"
end

local function is_directory(path)
  local stat = path and vim.uv.fs_stat(path) or nil
  return stat ~= nil and stat.type == "directory"
end

local function real(path)
  return vim.uv.fs_realpath(path) or vim.fs.normalize(path)
end

local function first_file(...)
  for index = 1, select("#", ...) do
    local path = select(index, ...)
    if path and is_file(path) then
      return real(path)
    end
  end
  return nil
end

local function first_directory(...)
  for index = 1, select("#", ...) do
    local path = select(index, ...)
    if path and is_directory(path) then
      return real(path)
    end
  end
  return nil
end

local function resolve_executable(explicit, client_name, fallbacks)
  local candidates = {}
  if explicit then
    table.insert(candidates, explicit)
  elseif client_name:find("/", 1, true) then
    table.insert(candidates, client_name)
  else
    local from_path = vim.fn.exepath(client_name)
    if from_path ~= "" then
      table.insert(candidates, from_path)
    end
    vim.list_extend(candidates, fallbacks)
  end

  for _, candidate in ipairs(candidates) do
    local from_path = vim.fn.exepath(candidate)
    local resolved = from_path ~= "" and from_path or candidate
    if vim.fn.executable(resolved) == 1 then
      return real(resolved)
    end
  end
  fail(
    string.format(
      "cannot find executable for client '%s'; install it or pass the matching --*-command option",
      client_name
    )
  )
end

local function resolve_environment(options)
  local data = vim.fn.stdpath("data")
  options.gti_runtime = first_directory(
    options.gti_runtime,
    vim.fs.joinpath(data, "lazy", "gti"),
    repository_root
  )
  if not options.gti_runtime then
    fail("cannot find an installed GTI Neovim runtime; pass --gti-runtime")
  end

  options.cpp_query_runtime = first_directory(
    vim.fs.joinpath(data, "lazy", "nvim-treesitter", "runtime"),
    vim.fs.joinpath(data, "lazy", "nvim-treesitter")
  )
  if not options.cpp_query_runtime then
    fail("cannot find the installed nvim-treesitter runtime containing C++ queries")
  end

  options.theme_runtime = first_directory(vim.fs.joinpath(data, "lazy", "github_theme"))
  if not options.theme_runtime then
    fail("cannot find the installed github_theme runtime")
  end

  options.cpp_parser = first_file(
    options.cpp_parser,
    vim.fs.joinpath(data, "site", "parser", "cpp.so")
  )
  if not options.cpp_parser then
    fail("cannot find the C++ Tree-sitter parser; pass --cpp-parser")
  end

  options.gti_parser = first_file(
    options.gti_parser,
    vim.fs.joinpath(options.gti_runtime, "toolchain", "share", "gti", "parser", "gti.so"),
    vim.fs.joinpath(repository_root, "build", "gti.so")
  )
  if not options.gti_parser then
    fail("cannot find the GTI Tree-sitter parser; pass --gti-parser")
  end

  options.cpp_command = resolve_executable(options.cpp_command, options.cpp_client, {})
  options.gti_command = resolve_executable(options.gti_command, options.gti_client, {
    vim.fs.joinpath(options.gti_runtime, "toolchain", "bin", "gti_lsp"),
    vim.fs.joinpath(repository_root, "build", "gti_lsp"),
  })

  options.cpp_client_name = vim.fs.basename(options.cpp_client)
  options.gti_client_name = vim.fs.basename(options.gti_client)
  options.output = vim.fs.abspath(options.output)
  options.cpp_fixture = vim.fs.abspath(options.cpp_fixture)
  options.gti_fixture = vim.fs.abspath(options.gti_fixture)

  if not is_file(options.cpp_fixture) then
    fail("C++ fixture does not exist: " .. options.cpp_fixture)
  end
  if not is_file(options.gti_fixture) then
    fail("GTI fixture does not exist: " .. options.gti_fixture)
  end
end

local function prepend_runtime(path)
  if path and not vim.list_contains(vim.opt.runtimepath:get(), path) then
    vim.opt.runtimepath:prepend(path)
  end
end

local function configure_runtime(options)
  prepend_runtime(options.cpp_query_runtime)
  prepend_runtime(options.gti_runtime)
  prepend_runtime(options.theme_runtime)

  local loaded_theme, theme = pcall(require, "github_theme")
  if not loaded_theme then
    fail("failed to load github_theme: " .. tostring(theme))
  end
  theme.setup({})
  local colored, color_error = pcall(vim.cmd.colorscheme, "github")
  if not colored or vim.g.colors_name ~= "github" then
    fail("failed to load github colorscheme: " .. tostring(color_error))
  end

  vim.env.GTI_TREE_SITTER_PATH = options.gti_parser
  vim.env.GTI_LSP_PATH = options.gti_command
  local original_enable = vim.lsp.enable
  vim.lsp.enable = function() end
  vim.g.loaded_gti = nil
  local plugin_path = vim.fs.joinpath(options.gti_runtime, "plugin", "gti.lua")
  local loaded_plugin, plugin_error = pcall(dofile, plugin_path)
  vim.lsp.enable = original_enable
  if not loaded_plugin then
    fail("failed to load the installed GTI Neovim plugin: " .. tostring(plugin_error))
  end

  local loaded_cpp, cpp_error = pcall(vim.treesitter.language.add, "cpp", {
    path = options.cpp_parser,
  })
  if not loaded_cpp then
    fail("failed to load C++ Tree-sitter parser: " .. tostring(cpp_error))
  end
  local loaded_gti, gti_error = pcall(vim.treesitter.language.add, "gti", {
    path = options.gti_parser,
  })
  if not loaded_gti then
    fail("failed to load GTI Tree-sitter parser: " .. tostring(gti_error))
  end

  vim.filetype.add({ extension = { gti = "gti" } })
end

local function parse_probe_line(line)
  local suffix, label, target = line:match(
    "^%s*//%s*@probe([%-a-z]*)%s+([%w%._%-]+)%s*|%s*(.-)%s*$"
  )
  if not label then
    return nil
  end
  local language = suffix == "-cpp" and "cpp" or suffix == "-gti" and "gti" or nil
  if suffix ~= "" and not language then
    fail("unknown probe marker @probe" .. suffix)
  end
  if target == "" then
    fail("probe '" .. label .. "' has an empty target")
  end
  return {
    label = label,
    target = target,
    language = language,
  }
end

local function target_occurrences(line, target)
  local occurrences = {}
  local start = 1
  local word_target = target:match("^[%w_]+$") ~= nil
  while true do
    local first, last = line:find(target, start, true)
    if not first then
      break
    end
    local before = first > 1 and line:sub(first - 1, first - 1) or ""
    local after = last < #line and line:sub(last + 1, last + 1) or ""
    local word_boundary = not word_target
      or (not before:match("[%w_]") and not after:match("[%w_]"))
    if word_boundary then
      table.insert(occurrences, { first = first, last = last })
    end
    start = last + 1
  end
  return occurrences
end

local function read_probes(path, language)
  local lines = vim.fn.readfile(path)
  local probes = {}
  local labels = {}
  for line_number, line in ipairs(lines) do
    local marker = parse_probe_line(line)
    if marker and (not marker.language or marker.language == language) then
      if labels[marker.label] then
        fail(string.format("duplicate probe '%s' in %s", marker.label, path))
      end
      local target_line = line_number + 1
      while target_line <= #lines
        and (lines[target_line]:match("^%s*$") or lines[target_line]:match("^%s*//"))
      do
        target_line = target_line + 1
      end
      if target_line > #lines then
        fail(string.format("probe '%s' has no following source line in %s", marker.label, path))
      end
      local occurrences = target_occurrences(lines[target_line], marker.target)
      if #occurrences ~= 1 then
        fail(string.format(
          "probe '%s' target '%s' occurs %d times on %s:%d; make the target unique",
          marker.label,
          marker.target,
          #occurrences,
          path,
          target_line
        ))
      end
      local occurrence = occurrences[1]
      labels[marker.label] = true
      table.insert(probes, {
        label = marker.label,
        shared = marker.language == nil,
        symbol_text = marker.target,
        row = target_line - 1,
        column = occurrence.first - 1,
        end_column = occurrence.last,
      })
    end
  end
  if #probes == 0 then
    fail("fixture contains no probes: " .. path)
  end
  table.sort(probes, function(left, right)
    return left.label < right.label
  end)
  return probes
end

local function wait_for(timeout, message, predicate)
  local ok = vim.wait(timeout, predicate, 20, false)
  if not ok then
    fail(message .. string.format(" (timed out after %d ms)", timeout))
  end
end

local function normalize_color(value)
  if type(value) == "number" then
    return string.format("#%06x", value)
  end
  return value
end

local visual_keys = {
  "fg",
  "bg",
  "sp",
  "bold",
  "italic",
  "underline",
  "undercurl",
  "underdouble",
  "underdotted",
  "underdashed",
  "strikethrough",
  "reverse",
  "standout",
  "nocombine",
}

local function normalize_attributes(attributes)
  local result = vim.empty_dict()
  for _, key in ipairs(visual_keys) do
    if attributes[key] ~= nil then
      local output_key = key == "fg" and "foreground"
        or key == "bg" and "background"
        or key == "sp" and "special"
        or key
      result[output_key] = normalize_color(attributes[key])
    end
  end
  return result
end

local function highlight_info(group)
  local direct_ok, direct = pcall(vim.api.nvim_get_hl, 0, {
    name = group,
    link = true,
    create = false,
  })
  direct = direct_ok and direct or {}
  local resolved = {}
  local resolved_from = group
  local candidate = group
  while candidate do
    local resolved_ok, attributes = pcall(vim.api.nvim_get_hl, 0, {
      name = candidate,
      link = false,
      create = false,
    })
    if resolved_ok and next(attributes) ~= nil then
      resolved = attributes
      resolved_from = candidate
      break
    end
    candidate = candidate:match("^(.*)%.[^.]+$")
  end
  return {
    group = group,
    direct_link = direct.link or vim.NIL,
    direct_attributes = normalize_attributes(direct),
    resolved_from = resolved_from,
    resolved_attributes = normalize_attributes(resolved),
  }
end

local function table_has_values(value)
  return next(value) ~= nil
end

local function tree_node_data(bufnr, language, probe)
  local node = vim.treesitter.get_node({
    bufnr = bufnr,
    pos = { probe.row, probe.column },
    lang = language,
    include_anonymous = true,
    ignore_injections = true,
  })
  if not node then
    fail(string.format("Tree-sitter returned no node for probe '%s'", probe.label))
  end
  local start_row, start_col, end_row, end_col = node:range()
  local ancestors = {}
  local current = node
  while current do
    table.insert(ancestors, current:type())
    current = current:parent()
  end
  return {
    type = node:type(),
    named = node:named(),
    range = {
      start = { row = start_row, column = start_col },
      finish = { row = end_row, column = end_col },
    },
    ancestors = ancestors,
  }
end

local function capture_priority(capture)
  local metadata = capture.metadata or {}
  local capture_metadata = metadata[capture.id] or {}
  return tonumber(metadata.priority or capture_metadata.priority)
    or vim.hl.priorities.treesitter
end

local function captures_at_probe(bufnr, filetype, probe)
  local captures = {}
  for _, capture in ipairs(vim.treesitter.get_captures_at_pos(bufnr, probe.row, probe.column)) do
    local group = "@" .. capture.capture .. "." .. filetype
    table.insert(captures, {
      capture = capture.capture,
      language = capture.lang,
      priority = capture_priority(capture),
      capture_id = capture.id,
      pattern_id = capture.pattern_id,
      highlight = highlight_info(group),
    })
  end
  table.sort(captures, function(left, right)
    if left.priority ~= right.priority then
      return left.priority < right.priority
    end
    if left.capture ~= right.capture then
      return left.capture < right.capture
    end
    return left.pattern_id < right.pattern_id
  end)
  return captures
end

local function modifier_list(modifiers)
  local result = {}
  for modifier, enabled in pairs(modifiers or {}) do
    if enabled then
      table.insert(result, modifier)
    end
  end
  table.sort(result)
  return result
end

local function selected_tokens_at_probe(bufnr, client_id, probe)
  local result = {}
  for _, token in ipairs(vim.lsp.semantic_tokens.get_at_pos(bufnr, probe.row, probe.column) or {}) do
    local starts_before = token.line < probe.row
      or (token.line == probe.row and token.start_col <= probe.column)
    local ends_after = token.end_line > probe.row
      or (token.end_line == probe.row and token.end_col > probe.column)
    if token.client_id == client_id and starts_before and ends_after then
      table.insert(result, {
        type = token.type,
        modifiers = modifier_list(token.modifiers),
        range = {
          start = { row = token.line, column = token.start_col },
          finish = { row = token.end_line, column = token.end_col },
        },
      })
    end
  end
  table.sort(result, function(left, right)
    if left.range.start.row ~= right.range.start.row then
      return left.range.start.row < right.range.start.row
    end
    if left.range.start.column ~= right.range.start.column then
      return left.range.start.column < right.range.start.column
    end
    return left.type < right.type
  end)
  return result
end

local function semantic_groups(filetype, token)
  if not token then
    return {}
  end
  local groups = {
    {
      kind = "type",
      priority = vim.hl.priorities.semantic_tokens,
      highlight = highlight_info(string.format("@lsp.type.%s.%s", token.type, filetype)),
    },
  }
  for _, modifier in ipairs(token.modifiers) do
    table.insert(groups, {
      kind = "modifier",
      modifier = modifier,
      priority = vim.hl.priorities.semantic_tokens + 1,
      highlight = highlight_info(string.format("@lsp.mod.%s.%s", modifier, filetype)),
    })
    table.insert(groups, {
      kind = "type_modifier",
      modifier = modifier,
      priority = vim.hl.priorities.semantic_tokens + 2,
      highlight = highlight_info(
        string.format("@lsp.typemod.%s.%s.%s", token.type, modifier, filetype)
      ),
    })
  end
  return groups
end

local function final_visual(captures, groups)
  local normal = highlight_info("Normal").resolved_attributes
  local attributes = vim.deepcopy(normal)
  local candidates = {}
  for _, capture in ipairs(captures) do
    table.insert(candidates, {
      source = "treesitter",
      role = capture.capture,
      priority = capture.priority,
      highlight = capture.highlight,
    })
  end
  for _, group in ipairs(groups) do
    table.insert(candidates, {
      source = "lsp",
      role = group.kind .. (group.modifier and ":" .. group.modifier or ""),
      priority = group.priority,
      highlight = group.highlight,
    })
  end
  table.sort(candidates, function(left, right)
    if left.priority ~= right.priority then
      return left.priority < right.priority
    end
    if left.source ~= right.source then
      return left.source < right.source
    end
    return left.role < right.role
  end)

  local contributors = {}
  for _, candidate in ipairs(candidates) do
    local resolved = candidate.highlight.resolved_attributes
    if table_has_values(resolved) then
      for key, value in pairs(resolved) do
        attributes[key] = value
      end
      table.insert(contributors, {
        source = candidate.source,
        role = candidate.role,
        group = candidate.highlight.group,
        direct_link = candidate.highlight.direct_link,
        priority = candidate.priority,
        resolved_attributes = resolved,
      })
    end
  end

  return {
    attributes = attributes,
    contributors = contributors,
  }
end

local function client_has_probe_token(bufnr, client_id, probes)
  for _, probe in ipairs(probes) do
    if #selected_tokens_at_probe(bufnr, client_id, probe) > 0 then
      return true
    end
  end
  return false
end

local function inspect_fixture(configuration, options)
  local probes = read_probes(configuration.path, configuration.language)
  vim.cmd("edit " .. vim.fn.fnameescape(configuration.path))
  local bufnr = vim.api.nvim_get_current_buf()

  wait_for(options.timeout, "filetype detection failed for " .. configuration.path, function()
    return vim.bo[bufnr].filetype == configuration.filetype
  end)

  local parser_ok, parser_or_error = pcall(vim.treesitter.get_parser, bufnr, configuration.language, {})
  if not parser_ok then
    fail("Tree-sitter parser unavailable for " .. configuration.language .. ": " .. tostring(parser_or_error))
  end
  local parser = parser_or_error
  local trees = parser:parse()
  if not trees or not trees[1] then
    fail("Tree-sitter did not produce a tree for " .. configuration.path)
  end
  if trees[1]:root():has_error() then
    fail("Tree-sitter found ERROR or MISSING syntax in " .. configuration.path)
  end
  local started, start_error = pcall(vim.treesitter.start, bufnr, configuration.language)
  if not started then
    fail("Tree-sitter highlighter failed for " .. configuration.language .. ": " .. tostring(start_error))
  end
  wait_for(options.timeout, "Tree-sitter highlighter did not become active", function()
    return vim.treesitter.highlighter.active[bufnr] ~= nil
  end)

  local client_id = vim.lsp.start({
    name = configuration.client_name,
    cmd = { configuration.command },
    root_dir = vim.fs.dirname(configuration.path),
    capabilities = vim.lsp.protocol.make_client_capabilities(),
  }, {
    bufnr = bufnr,
    reuse_client = function()
      return false
    end,
  })
  if not client_id then
    fail("failed to start LSP client " .. configuration.client_name)
  end

  wait_for(options.timeout, "LSP client did not attach: " .. configuration.client_name, function()
    local client = vim.lsp.get_client_by_id(client_id)
    return client ~= nil
      and client.initialized
      and vim.lsp.buf_is_attached(bufnr, client_id)
  end)
  local client = vim.lsp.get_client_by_id(client_id)
  if client.name ~= configuration.client_name then
    fail(string.format(
      "expected LSP client '%s' but attached '%s'",
      configuration.client_name,
      client.name
    ))
  end
  if not client.server_capabilities.semanticTokensProvider then
    fail("LSP client does not advertise semantic tokens: " .. configuration.client_name)
  end

  vim.lsp.semantic_tokens.enable(true, { client_id = client_id })
  vim.lsp.semantic_tokens.force_refresh(bufnr)
  wait_for(options.timeout, "semantic tokens did not become available from " .. client.name, function()
    return client_has_probe_token(bufnr, client_id, probes)
  end)
  vim.cmd("redraw")

  local records = {}
  for _, probe in ipairs(probes) do
    local captures = captures_at_probe(bufnr, configuration.filetype, probe)
    local semantic_tokens = selected_tokens_at_probe(bufnr, client_id, probe)
    local semantic_token = semantic_tokens[1]
    local groups = semantic_groups(configuration.filetype, semantic_token)
    table.insert(records, {
      label = probe.label,
      shared = probe.shared,
      source = vim.fs.basename(configuration.path),
      filetype = configuration.filetype,
      symbol_text = probe.symbol_text,
      position = {
        row = probe.row,
        byte_column = probe.column,
      },
      byte_range = {
        start = { row = probe.row, column = probe.column },
        finish = { row = probe.row, column = probe.end_column },
      },
      treesitter = {
        node = tree_node_data(bufnr, configuration.language, probe),
        captures = captures,
      },
      lsp = {
        selected_client = {
          name = client.name,
          client_id = client_id,
          server_version = client.server_info and client.server_info.version or vim.NIL,
        },
        semantic_tokens = semantic_tokens,
        semantic_token = semantic_token or vim.NIL,
        derived_highlight_groups = groups,
      },
      final_visual = final_visual(captures, groups),
    })
  end

  local report = {
    schema_version = 1,
    benchmark = configuration.language == "cpp" and "cpp" or "cpp-target",
    theme = "github",
    source = configuration.path,
    filetype = configuration.filetype,
    parser = {
      language = configuration.language,
      path = configuration.parser,
    },
    lsp = {
      client_name = client.name,
      client_id = client_id,
      executable = configuration.command,
      server_version = client.server_info and client.server_info.version or vim.NIL,
    },
    probes = records,
  }

  client:stop()
  vim.wait(1000, function()
    return vim.lsp.get_client_by_id(client_id) == nil
  end, 20, false)
  vim.api.nvim_buf_delete(bufnr, { force = true })
  return report
end

local function write_report(path, value)
  local json = compare.canonical_json(value)
  local lines = vim.split(json, "\n", { plain = true })
  if lines[#lines] == "" then
    table.remove(lines)
  end
  local ok = vim.fn.writefile(lines, path)
  if ok ~= 0 then
    fail("failed to write report: " .. path)
  end
end

local function print_summary(comparison, output)
  local width = 0
  for _, row in ipairs(comparison.probes) do
    width = math.max(width, #row.label)
  end
  local lines = {
    string.format("%-" .. width .. "s  %s", "PROBE", "STATUS"),
  }
  for _, row in ipairs(comparison.probes) do
    local intentional = row.intentional_differences or {}
    local suffix = #intentional > 0 and " (intentional semantic difference)" or ""
    table.insert(lines, string.format("%-" .. width .. "s  %s%s", row.label, row.status, suffix))
  end
  table.insert(lines, "")
  table.insert(lines, string.format(
    "C++ benchmark: %d shared probes, %d visual mismatches, %d missing probes",
    #comparison.probes,
    comparison.unexpected_visual_mismatches,
    comparison.missing_shared_probes
  ))
  table.insert(lines, "Reports: " .. output)
  io.stdout:write(table.concat(lines, "\n") .. "\n")
end

local function main()
  local options = parse_options(arg or {})
  if options.help then
    io.stdout:write(usage())
    return 0
  end

  resolve_environment(options)
  configure_runtime(options)
  vim.fn.mkdir(options.output, "p")

  local cpp = inspect_fixture({
    language = "cpp",
    filetype = "cpp",
    path = options.cpp_fixture,
    parser = options.cpp_parser,
    client_name = options.cpp_client_name,
    command = options.cpp_command,
  }, options)
  local gti = inspect_fixture({
    language = "gti",
    filetype = "gti",
    path = options.gti_fixture,
    parser = options.gti_parser,
    client_name = options.gti_client_name,
    command = options.gti_command,
  }, options)
  local comparison = compare.compare(cpp, gti)

  write_report(vim.fs.joinpath(options.output, "cpp.json"), cpp)
  write_report(vim.fs.joinpath(options.output, "gti.json"), gti)
  write_report(vim.fs.joinpath(options.output, "comparison.json"), comparison)
  print_summary(comparison, options.output)

  if comparison.missing_shared_probes > 0 then
    return 1
  end
  if options.fail_on_visual_difference and comparison.unexpected_visual_mismatches > 0 then
    return 2
  end
  return 0
end

local ok, result = xpcall(main, function(problem)
  return tostring(problem)
end)
if not ok then
  io.stderr:write("highlight-inspector: " .. result .. "\n")
  os.exit(1)
end
os.exit(result)
