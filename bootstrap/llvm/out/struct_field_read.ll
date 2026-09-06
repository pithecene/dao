%struct.P = type { i32, i32 }

define i32 @get_x(%struct.P %arg0) {
entry:
  %slot0 = alloca %struct.P
  store %struct.P %arg0, ptr %slot0
  %0 = load %struct.P, ptr %slot0
  %1 = extractvalue %struct.P %0, 0
  ret i32 %1
}
