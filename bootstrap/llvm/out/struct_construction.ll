%struct.P = type { i32, i32 }

define i32 @main() {
entry:
  %slot0 = alloca %struct.P
  %0 = alloca %struct.P
  %1 = getelementptr inbounds %struct.P, ptr %0, i32 0, i32 0
  store i32 1, ptr %1
  %2 = getelementptr inbounds %struct.P, ptr %0, i32 0, i32 1
  store i32 2, ptr %2
  %3 = load %struct.P, ptr %0
  store %struct.P %3, ptr %slot0
  %4 = load %struct.P, ptr %slot0
  %5 = extractvalue %struct.P %4, 0
  ret i32 %5
}
