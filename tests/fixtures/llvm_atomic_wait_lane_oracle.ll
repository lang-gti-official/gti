; Test-only physical-lane oracle, not GTI generated-body coverage.
; No std::atomic or std::atomic_ref object is created by this interface.
define void @gti_test_llvm_store8(ptr %address, i64 %bits) {
  %lane = trunc i64 %bits to i8
  store atomic i8 %lane, ptr %address release, align 1
  ret void
}
define void @gti_test_llvm_store16(ptr %address, i64 %bits) {
  %lane = trunc i64 %bits to i16
  store atomic i16 %lane, ptr %address release, align 2
  ret void
}
define void @gti_test_llvm_store32(ptr %address, i64 %bits) {
  %lane = trunc i64 %bits to i32
  store atomic i32 %lane, ptr %address release, align 4
  ret void
}
define void @gti_test_llvm_store64(ptr %address, i64 %bits) {
  store atomic i64 %bits, ptr %address release, align 8
  ret void
}
define i64 @gti_test_llvm_load8(ptr %address) {
  %lane = load atomic i8, ptr %address acquire, align 1
  %bits = zext i8 %lane to i64
  ret i64 %bits
}
define i64 @gti_test_llvm_load16(ptr %address) {
  %lane = load atomic i16, ptr %address acquire, align 2
  %bits = zext i16 %lane to i64
  ret i64 %bits
}
define i64 @gti_test_llvm_load32(ptr %address) {
  %lane = load atomic i32, ptr %address acquire, align 4
  %bits = zext i32 %lane to i64
  ret i64 %bits
}
define i64 @gti_test_llvm_load64(ptr %address) {
  %lane = load atomic i64, ptr %address acquire, align 8
  ret i64 %lane
}
