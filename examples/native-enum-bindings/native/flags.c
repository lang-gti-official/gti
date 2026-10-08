#include "flags.h"

struct FlagRequest flags_make(enum AccessFlags access, enum WideFlags mask,
                              enum SignedOffset offset) {
  struct FlagRequest request = {access, mask, offset};
  return request;
}

int flags_validate(struct FlagRequest request) {
  return request.access == ACCESS_HIGH && request.mask == WIDE_ALL &&
         request.offset == OFFSET_MIN;
}

enum WideFlags flags_echo(enum WideFlags value) { return value; }
enum SignedOffset flags_offset(enum SignedOffset value) { return value; }
void flags_write(enum WideFlags *output) { *output = WIDE_ALL; }
FlagTransform flags_transform(void) { return flags_echo; }
unsigned long long flags_request_size(void) {
  return sizeof(struct FlagRequest);
}
unsigned long long flags_request_alignment(void) {
  return _Alignof(struct FlagRequest);
}
