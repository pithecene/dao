%struct.P = type { i32, i32 }

define %struct.P @make() {
entry:
  %0 = alloca %struct.P
  %1 = getelementptr inbounds %struct.P, ptr %0, i32 0, i32 0
  store i32 5, ptr %1
  %2 = getelementptr inbounds %struct.P, ptr %0, i32 0, i32 1
  store i32 6, ptr %2
  %3 = load %struct.P, ptr %0
  ret %struct.P %3
}

define i32 @main() {
entry:
  %slot0 = alloca %struct.P
  %0 = call %struct.P @make()
  store %struct.P %0, ptr %slot0
  %1 = load %struct.P, ptr %slot0
  %2 = extractvalue %struct.P %1, 0
  ret i32 %2
}
