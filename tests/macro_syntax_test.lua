local repository = assert(vim.env.GTI_TEST_REPOSITORY, "GTI_TEST_REPOSITORY is required")
vim.opt.runtimepath:prepend(repository)
vim.cmd("syntax on")
vim.api.nvim_buf_set_lines(0, 0, -1, false, {
  "#macro statement(int value) { use(value); }",
  "#macro int choose(bool enabled, int first, int second) { return first; }",
  "[[track_caller]] #macro T generic<T>(T value) { return value; }",
  "#macro std::optional<T> maybe<T>(T value) { return value; }",
})
vim.bo.syntax = "gti"
for row, name in ipairs({ "statement", "choose", "generic", "maybe" }) do
  local line = vim.api.nvim_buf_get_lines(0, row - 1, row, false)[1]
  for token, expected in pairs({ ["#macro"] = "gtiMacroKeyword", [name] = "gtiMacroName" }) do
    local column = assert(line:find(token, 1, true))
    local actual = vim.fn.synIDattr(vim.fn.synID(row, column, true), "name")
    assert(actual == expected, token .. " classified as " .. actual .. ", expected " .. expected)
  end
end
vim.cmd("qa!")
