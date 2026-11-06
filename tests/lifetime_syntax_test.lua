local repository = assert(vim.env.GTI_TEST_REPOSITORY, "GTI_TEST_REPOSITORY is required")
vim.opt.runtimepath:prepend(repository)
vim.cmd("syntax on")
local lines = {
  "int&<a> identity<lifetime a>(int&<a> value);",
  "struct Pair<T, lifetime source> { T&<source> value; };",
  "int lifetime = 1;",
  "using Reader = for<lifetime a> int&<a>(int&<a>);",
}
vim.api.nvim_buf_set_lines(0, 0, -1, false, lines)
vim.bo.syntax = "gti"
local function check(row, word, expected, start)
  local column = assert(lines[row]:find(word, start or 1, true))
  local actual = vim.fn.synIDattr(vim.fn.synID(row, column, true), "name")
  assert(actual == expected, word .. " classified as " .. actual .. ", expected " .. expected)
  return column
end
check(1, "lifetime", "gtiLifetimeKeyword")
check(1, "a", "gtiLifetimeName", lines[1]:find("lifetime", 1, true) + #"lifetime")
check(1, "a", "gtiLifetimeArgument")
check(2, "lifetime", "gtiLifetimeKeyword")
check(2, "source", "gtiLifetimeName")
check(2, "source", "gtiLifetimeArgument", lines[2]:find("T&", 1, true))
check(4, "lifetime", "gtiLifetimeKeyword")
local ordinary = vim.fn.synIDattr(vim.fn.synID(3, lines[3]:find("lifetime", 1, true), true), "name")
assert(ordinary ~= "gtiLifetimeKeyword", "ordinary lifetime identifier became a keyword")
vim.cmd("qa!")
