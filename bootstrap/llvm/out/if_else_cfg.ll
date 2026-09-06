define i32 @f(i32 %arg0) {
entry:
  %slot0 = alloca i32
  store i32 %arg0, ptr %slot0
  %0 = load i32, ptr %slot0
  %1 = icmp sgt i32 %0, 0
  br i1 %1, label %bb1, label %bb2
bb1:
  ret i32 1
bb2:
  ret i32 2
bb3:
  unreachable
}
