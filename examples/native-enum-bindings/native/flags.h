#ifndef GTI_EXAMPLE_NATIVE_FLAGS_H
#define GTI_EXAMPLE_NATIVE_FLAGS_H

enum AccessFlags { ACCESS_READ = 1U, ACCESS_HIGH = 0x80000000U };
enum WideFlags {
  WIDE_HIGH = 0x8000000000000000ULL,
  WIDE_ALL = 0xffffffffffffffffULL
};
enum SignedOffset {
  OFFSET_MIN = (-9223372036854775807LL - 1),
  OFFSET_MAX = 9223372036854775807LL
};

struct FlagRequest {
  enum AccessFlags access;
  enum WideFlags mask;
  enum SignedOffset offset;
};

typedef enum WideFlags (*FlagTransform)(enum WideFlags value);

int flags_validate(struct FlagRequest request);
struct FlagRequest flags_make(enum AccessFlags access, enum WideFlags mask,
                              enum SignedOffset offset);
enum WideFlags flags_echo(enum WideFlags value);
enum SignedOffset flags_offset(enum SignedOffset value);
void flags_write(enum WideFlags *output);
FlagTransform flags_transform(void);
unsigned long long flags_request_size(void);
unsigned long long flags_request_alignment(void);

#endif
