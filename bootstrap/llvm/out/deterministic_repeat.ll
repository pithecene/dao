define i32 @main() {
entry:
  %slot0 = alloca i32
  %0 = add i32 1, 2
  store i32 %0, ptr %slot0
  %1 = load i32, ptr %slot0
  ret i32 %1
}
