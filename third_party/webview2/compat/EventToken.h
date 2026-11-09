#pragma once
// MinGW 的 Windows 头文件里该文件名为小写 eventtoken.h，
// 而 WebView2.h 按 Windows SDK 的大小写写作 EventToken.h。
// 本垫片仅供 MinGW 交叉编译时加入包含路径；用 Visual Studio 编译时
// 不要把 compat 目录加进包含路径，SDK 自带 EventToken.h。
#include <eventtoken.h>
