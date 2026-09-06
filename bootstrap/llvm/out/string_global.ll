@.str.0 = private unnamed_addr constant [3 x i8] c"hi\00"

declare i32 @puts(ptr %arg0)

define i32 @main() {
entry:
  %0 = getelementptr inbounds [3 x i8], ptr @.str.0, i64 0, i64 0
  %1 = call i32 @puts(ptr %0)
  ret i32 %1
}
