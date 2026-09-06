define i32 @f(i32 %arg0) {
entry:
  %slot0 = alloca i32
  %slot1 = alloca i32
  store i32 %arg0, ptr %slot0
  store i32 0, ptr %slot1
  br label %bb1
bb1:
  %0 = load i32, ptr %slot1
  %1 = load i32, ptr %slot0
  %2 = icmp slt i32 %0, %1
  br i1 %2, label %bb2, label %bb3
bb2:
  %3 = load i32, ptr %slot1
  %4 = add i32 %3, 1
  store i32 %4, ptr %slot1
  br label %bb1
bb3:
  %5 = load i32, ptr %slot1
  ret i32 %5
}
