// ============================================================================
//  log.h/.cpp — 极简日志：控制台(如果有) + 文件(与 exe 同目录)
// ============================================================================
#pragma once
#include <windows.h>

void  LogInit(const wchar_t* path /*NULL/空 = 与 exe 同目录 OSeasy-Tool.log*/);
void  LogClose(void);
void  Log(const char* fmt, ...);       // 自动加 [HH:MM:SS] 前缀并写文件
// 注意：Log() 是**窄字符** printf。不要把 `%ls` 和含中文的宽字符串一起用 ——
// 默认 "C" locale 下 wcstombs 转换失败，_vsnprintf_s 返回 -1，结果是整行输出为空。
// 要打中文请用 %s + UTF-8 窄字符串（源文件已经是 /utf-8 编译）。
void  LogW(const wchar_t* text);       // 直接写一行宽字符(少见)
const wchar_t* LogFilePath(void);      // 实际日志全路径（状态框/打开日志目录用）
