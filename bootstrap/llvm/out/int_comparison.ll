define i1 @f(i32 %arg0) {
entry:
  %slot0 = alloca i32
  store i32 %arg0, ptr %slot0
  %0 = load i32, ptr %slot0
  %1 = icmp slt i32 %0, 5
  ret i1 %1
}
