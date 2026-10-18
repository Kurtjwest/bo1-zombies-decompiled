// <shlobj.h> stand-in for the macOS port. Known folders map to the game folder (SHGetFolderPathA, kernel32_misc.cpp).
#pragma once
#include <windows.h>
#ifdef __cplusplus
extern "C" {
#endif
#define CSIDL_DESKTOP 0x0000
#define CSIDL_PERSONAL 0x0005
#define CSIDL_APPDATA 0x001a
#define CSIDL_LOCAL_APPDATA 0x001c
#define CSIDL_COMMON_APPDATA 0x0023
#define CSIDL_MYDOCUMENTS CSIDL_PERSONAL
#define CSIDL_FLAG_CREATE 0x8000
#define SHGFP_TYPE_CURRENT 0
#define SHGFP_TYPE_DEFAULT 1
HRESULT SHGetFolderPathA(HWND hwnd, int csidl, HANDLE hToken, DWORD dwFlags, LPSTR pszPath);
BOOL SHGetSpecialFolderPathA(HWND hwnd, LPSTR pszPath, int csidl, BOOL fCreate);
int SHCreateDirectoryExA(HWND hwnd, LPCSTR pszPath, const SECURITY_ATTRIBUTES *psa);
#define SHGetFolderPath SHGetFolderPathA
#define SHGetSpecialFolderPath SHGetSpecialFolderPathA
#define SHCreateDirectoryEx SHCreateDirectoryExA
#ifdef __cplusplus
}
#endif
