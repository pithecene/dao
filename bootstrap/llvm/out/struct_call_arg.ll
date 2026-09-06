%struct.P = type { i32, i32 }

define i32 @take(%struct.P %arg0) {
entry:
  %slot0 = alloca %struct.P
  store %struct.P %arg0, ptr %slot0
  %0 = load %struct.P, ptr %slot0
  %1 = extractvalue %struct.P %0, 0
  ret i32 %1
}

define i32 @main() {
entry:
  %slot0 = alloca %struct.P
  %0 = alloca %struct.P
  %1 = getelementptr inbounds %struct.P, ptr %0, i32 0, i32 0
  store i32 3, ptr %1
  %2 = getelementptr inbounds %struct.P, ptr %0, i32 0, i32 1
  store i32 4, ptr %2
  %3 = load %struct.P, ptr %0
  store %struct.P %3, ptr %slot0
  %4 = load %struct.P, ptr %slot0
  %5 = call i32 @take(%struct.P %4)
  ret i32 %5
}
