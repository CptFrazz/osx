// RUN: clang-cc -triple x86_64-apple-darwin-10 -emit-llvm -o %t %s &&
// RUN: grep -F '@"\01L_OBJC_IMAGE_INFO" = internal constant [2 x i32] [i32 0, i32 16], section "__OBJC, __image_info,regular"' %t
