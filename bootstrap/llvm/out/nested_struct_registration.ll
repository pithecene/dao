%struct.Outer = type { %struct.Inner }
%struct.Inner = type { i32 }

define i32 @main() {
entry:
  %slot0 = alloca %struct.Outer
  %0 = alloca %struct.Inner
  %1 = getelementptr inbounds %struct.Inner, ptr %0, i32 0, i32 0
  store i32 42, ptr %1
  %2 = load %struct.Inner, ptr %0
  %3 = alloca %struct.Outer
  %4 = getelementptr inbounds %struct.Outer, ptr %3, i32 0, i32 0
  store %struct.Inner %2, ptr %4
  %5 = load %struct.Outer, ptr %3
  store %struct.Outer %5, ptr %slot0
  ret i32 0
}
