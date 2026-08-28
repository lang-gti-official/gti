#include "gti/mir_editor.h"

int mirEditorHeaderSmoke() {
  lang::MirProgram program;
  const lang::MirEditor editor(program, "header-smoke");
  return editor.pendingPatchCount() == 0 ? 0 : 1;
}
