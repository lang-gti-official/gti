#include "gti/semantic_analyzer.h"

int semanticCoreHeaderSmoke();
int semanticModelHeaderSmoke();

int main() {
  lang::SemanticVisitor visitor;
  return visitor.hadError()
             ? 1
             : semanticCoreHeaderSmoke() | semanticModelHeaderSmoke();
}
