# Installs the LLVM header roots (`llvm`, `llvm-c`) found under each of
# `include_dirs` into `<includedir>/gti-llvm/<root>` for the SDK component.
#
# Distribution packages often ship a header root as a relative symlink, such
# as Ubuntu's `/usr/lib/llvm-23/include/llvm -> ../../../include/llvm-23/llvm`.
# Installing that path with install(DIRECTORY) keeps the link itself, which no
# longer resolves once staged or relocated. Each root is therefore resolved to
# its real directory first, and its contents are installed under the root's own
# name, so the SDK always holds real headers.
function(gti_install_llvm_headers include_dirs component)
  foreach(llvm_include IN LISTS include_dirs)
    foreach(llvm_header_root IN ITEMS llvm llvm-c)
      if(EXISTS "${llvm_include}/${llvm_header_root}")
        get_filename_component(llvm_header_dir
          "${llvm_include}/${llvm_header_root}" REALPATH)
        install(DIRECTORY "${llvm_header_dir}/"
          DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/gti-llvm/${llvm_header_root}"
          COMPONENT "${component}" FILES_MATCHING PATTERN "*.h" PATTERN "*.inc"
          PATTERN "CMakeFiles" EXCLUDE)
      endif()
    endforeach()
  endforeach()
endfunction()
