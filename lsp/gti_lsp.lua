local toolchain = require("gti.toolchain")

return {
  cmd = { toolchain.executable("gti_lsp") },
  filetypes = { "gti" },
  root_markers = { ".git", "CMakeLists.txt" },
}
