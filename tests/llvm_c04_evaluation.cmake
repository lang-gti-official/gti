# Experimental copies only: never linked into gti, exported, installed, or
# included in the default test suite. Both share the unchanged frontend library.
option(GTI_BUILD_LLVM_C04_EVALUATION "Build isolated MIR failure-transport experiments" OFF)
if(NOT GTI_BUILD_LLVM_C04_EVALUATION)
  return()
endif()
get_target_property(c04_backend_sources gti_llvm_backend SOURCES)
list(TRANSFORM c04_backend_sources PREPEND "${PROJECT_SOURCE_DIR}/")
foreach(mode IN ITEMS status unwind)
  add_library(gti_c04_${mode}_backend STATIC EXCLUDE_FROM_ALL
    ${c04_backend_sources} llvm_c04_admission.cpp llvm_c04_workers.cpp)
  target_include_directories(gti_c04_${mode}_backend PUBLIC
    "${PROJECT_SOURCE_DIR}/include")
  target_include_directories(gti_c04_${mode}_backend SYSTEM PRIVATE ${GTI_LLVM_INCLUDE_DIRS})
  target_compile_features(gti_c04_${mode}_backend PUBLIC cxx_std_20)
  target_compile_definitions(gti_c04_${mode}_backend PRIVATE GTI_LLVM_C04_MIR_EVALUATION=1)
  if(mode STREQUAL "unwind")
    target_compile_definitions(gti_c04_${mode}_backend PRIVATE GTI_LLVM_C04_NATIVE_UNWIND=1)
  endif()
  target_link_libraries(gti_c04_${mode}_backend PUBLIC gti_compiler
    PRIVATE ${GTI_LLVM_BACKEND_DIRECT_LIBRARIES})
  add_executable(gti_c04_${mode}_emitter EXCLUDE_FROM_ALL llvm_c04_mir_evaluation.cpp)
  target_compile_definitions(gti_c04_${mode}_emitter PRIVATE
    GTI_LLVM_C04_MIR_EVALUATION=1 "GTI_C04_MODE=\"${mode}\"")
  target_include_directories(gti_c04_${mode}_emitter SYSTEM PRIVATE ${GTI_LLVM_INCLUDE_DIRS})
  target_include_directories(gti_c04_${mode}_emitter PRIVATE "${PROJECT_SOURCE_DIR}/runtime/include")
  target_link_libraries(gti_c04_${mode}_emitter PRIVATE gti_c04_${mode}_backend)
endforeach()
