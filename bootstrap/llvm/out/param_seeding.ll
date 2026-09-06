define i32 @f(i32 %arg0) {
entry:
  %slot0 = alloca i32
  store i32 %arg0, ptr %slot0
  %0 = load i32, ptr %slot0
  ret i32 %0
}
