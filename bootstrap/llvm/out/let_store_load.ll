define i32 @main() {
entry:
  %slot0 = alloca i32
  store i32 1, ptr %slot0
  %0 = load i32, ptr %slot0
  ret i32 %0
}
