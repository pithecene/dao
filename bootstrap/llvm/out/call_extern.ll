define i32 @add(i32 %arg0, i32 %arg1) {
entry:
  %slot0 = alloca i32
  %slot1 = alloca i32
  store i32 %arg0, ptr %slot0
  store i32 %arg1, ptr %slot1
  %0 = load i32, ptr %slot0
  %1 = load i32, ptr %slot1
  %2 = add i32 %0, %1
  ret i32 %2
}

define i32 @main() {
entry:
  %0 = call i32 @add(i32 1, i32 2)
  ret i32 %0
}
