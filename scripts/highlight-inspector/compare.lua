local M = {}

M.allowlist = {
  {
    kind = "semantic_modifier",
    side = "gti",
    modifier = "readonly",
    labels = {
      "parameter.*",
      "local.readonly.*",
      "constant.*",
    },
    reason = "GTI bindings and parameters are readonly by default.",
  },
}

local function sorted_keys(value)
  local keys = {}
  for key in pairs(value) do
    table.insert(keys, key)
  end
  table.sort(keys, function(left, right)
    return tostring(left) < tostring(right)
  end)
  return keys
end

local function encode_json(value, depth)
  depth = depth or 0
  local kind = type(value)
  if value == nil or value == vim.NIL then
    return "null"
  end
  if kind == "boolean" or kind == "number" then
    return tostring(value)
  end
  if kind == "string" then
    return vim.json.encode(value)
  end
  if kind ~= "table" then
    error("cannot encode JSON value of type " .. kind)
  end

  local prefix = string.rep("  ", depth)
  local child_prefix = string.rep("  ", depth + 1)
  if vim.isarray(value) then
    if #value == 0 then
      return "[]"
    end
    local items = {}
    for _, item in ipairs(value) do
      table.insert(items, child_prefix .. encode_json(item, depth + 1))
    end
    return "[\n" .. table.concat(items, ",\n") .. "\n" .. prefix .. "]"
  end

  local keys = sorted_keys(value)
  if #keys == 0 then
    return "{}"
  end
  local items = {}
  for _, key in ipairs(keys) do
    if type(key) ~= "string" then
      error("JSON object keys must be strings")
    end
    table.insert(
      items,
      child_prefix .. vim.json.encode(key) .. ": " .. encode_json(value[key], depth + 1)
    )
  end
  return "{\n" .. table.concat(items, ",\n") .. "\n" .. prefix .. "}"
end

function M.canonical_json(value)
  return encode_json(value, 0) .. "\n"
end

local function signatures_equal(left, right)
  return encode_json(left, 0) == encode_json(right, 0)
end

local function capture_signature(record)
  local result = {}
  local seen = {}
  for _, capture in ipairs(record.treesitter.captures or {}) do
    local key = capture.capture .. "\0" .. tostring(capture.priority)
    if not seen[key] then
      seen[key] = true
      table.insert(result, {
        capture = capture.capture,
        priority = capture.priority,
      })
    end
  end
  table.sort(result, function(left, right)
    if left.capture ~= right.capture then
      return left.capture < right.capture
    end
    return left.priority < right.priority
  end)
  return result
end

local function semantic_signature(record)
  local token = record.lsp.semantic_token
  if type(token) ~= "table" then
    return {
      type = vim.NIL,
      modifiers = {},
    }
  end
  return {
    type = token.type,
    modifiers = token.modifiers,
  }
end

local function modifier_set(modifiers)
  local result = {}
  for _, modifier in ipairs(modifiers or {}) do
    result[modifier] = true
  end
  return result
end

local function label_matches(pattern, label)
  local escaped = pattern:gsub("([^%w])", "%%%1"):gsub("%%%*", ".*")
  return label:match("^" .. escaped .. "$") ~= nil
end

local function allowed_modifier_difference(label, cpp, gti)
  local cpp_set = modifier_set(cpp)
  local gti_set = modifier_set(gti)
  local extras = {}
  local missing = {}
  for modifier in pairs(gti_set) do
    if not cpp_set[modifier] then
      table.insert(extras, modifier)
    end
  end
  for modifier in pairs(cpp_set) do
    if not gti_set[modifier] then
      table.insert(missing, modifier)
    end
  end
  table.sort(extras)
  table.sort(missing)

  for _, rule in ipairs(M.allowlist) do
    local matched_label = false
    for _, pattern in ipairs(rule.labels) do
      matched_label = matched_label or label_matches(pattern, label)
    end
    if matched_label
      and rule.kind == "semantic_modifier"
      and rule.side == "gti"
      and #extras == 1
      and extras[1] == rule.modifier
      and #missing == 0
    then
      return true, rule.reason
    end
  end
  return false, nil
end

local function index_records(report)
  local index = {}
  for _, record in ipairs(report.probes) do
    index[record.label] = record
  end
  return index
end

local function visual_signature(record)
  return record.final_visual and record.final_visual.attributes or vim.NIL
end

local function increment(summary, status)
  summary[status] = (summary[status] or 0) + 1
end

function M.compare(cpp_report, gti_report)
  local cpp = index_records(cpp_report)
  local gti = index_records(gti_report)
  local shared_labels = {}
  local language_specific = {
    cpp = {},
    gti = {},
  }

  for _, record in ipairs(cpp_report.probes) do
    if record.shared then
      shared_labels[record.label] = true
    else
      table.insert(language_specific.cpp, record.label)
    end
  end
  for _, record in ipairs(gti_report.probes) do
    if record.shared then
      shared_labels[record.label] = true
    else
      table.insert(language_specific.gti, record.label)
    end
  end

  local labels = sorted_keys(shared_labels)
  table.sort(language_specific.cpp)
  table.sort(language_specific.gti)
  local records = {}
  local summary = vim.empty_dict()
  local missing = 0
  local visual_mismatches = 0

  for _, label in ipairs(labels) do
    local left = cpp[label]
    local right = gti[label]
    if not left or not right then
      local row = {
        label = label,
        status = "missing probe",
        cpp = left or vim.NIL,
        gti = right or vim.NIL,
      }
      table.insert(records, row)
      increment(summary, row.status)
      missing = missing + 1
    else
      local left_ts = capture_signature(left)
      local right_ts = capture_signature(right)
      local left_semantic = semantic_signature(left)
      local right_semantic = semantic_signature(right)
      local tree_match = signatures_equal(left_ts, right_ts)
      local type_match = left_semantic.type == right_semantic.type
      local modifier_match = signatures_equal(left_semantic.modifiers, right_semantic.modifiers)
      local modifier_allowed, allow_reason = false, nil
      if not modifier_match then
        modifier_allowed, allow_reason = allowed_modifier_difference(
          label,
          left_semantic.modifiers,
          right_semantic.modifiers
        )
      end
      local visual_match = signatures_equal(visual_signature(left), visual_signature(right))

      local status
      if not visual_match then
        status = "visual mismatch"
        visual_mismatches = visual_mismatches + 1
      elseif not type_match or not modifier_match then
        status = "semantic difference, visual match"
      elseif not tree_match then
        status = "Tree-sitter difference"
      else
        status = "exact match"
      end

      local intentional = {}
      if modifier_allowed then
        table.insert(intentional, {
          kind = "semantic_modifier",
          reason = allow_reason,
        })
      end

      local row = {
        label = label,
        status = status,
        intentional_differences = intentional,
        differences = {
          treesitter = {
            match = tree_match,
            cpp = left_ts,
            gti = right_ts,
          },
          semantic_type = {
            match = type_match,
            cpp = left_semantic.type,
            gti = right_semantic.type,
          },
          semantic_modifiers = {
            match = modifier_match,
            allowed = modifier_allowed,
            cpp = left_semantic.modifiers,
            gti = right_semantic.modifiers,
          },
          visual = {
            match = visual_match,
            cpp = visual_signature(left),
            gti = visual_signature(right),
          },
        },
        cpp = left,
        gti = right,
      }
      table.insert(records, row)
      increment(summary, status)
    end
  end

  return {
    schema_version = 1,
    benchmark = "cpp",
    benchmark_rule = "Equivalent GTI constructs should resolve to the same color and style as C++.",
    theme = cpp_report.theme,
    allowlist = M.allowlist,
    summary = summary,
    missing_shared_probes = missing,
    unexpected_visual_mismatches = visual_mismatches,
    language_specific_probes = language_specific,
    probes = records,
  }
end

return M
