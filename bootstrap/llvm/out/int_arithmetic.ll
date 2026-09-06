define i32 @main() {
entry:
  %slot0 = alloca i32
  %0 = mul i32 2, 3
  %1 = add i32 1, %0
  store i32 %1, ptr %slot0
  %2 = load i32, ptr %slot0
  ret i32 %2
}
