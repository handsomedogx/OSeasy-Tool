// ============================================================================
//  log.h/.cpp — 极简日志：控制台(如果有) + 文件(与 exe 同目录)
// ============================================================================
#pragma once
#include <windows.h>

void  LogInit(const wchar_t* path /*NULL/空 = 与 exe 同目录 OeWinFix2.log*/);
void  LogClose(void);
void  Log(const char* fmt, ...);       // 自动加 [HH:MM:SS] 前缀并写文件
void  LogW(const wchar_t* text);       // 直接写一行宽字符(少见)
const wchar_t* LogFilePath(void);      // 实际日志全路径（状态框/打开日志目录用）
