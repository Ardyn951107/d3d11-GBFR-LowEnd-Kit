#include <windows.h>
#include <stdio.h>
int main(){ wchar_t me[MAX_PATH]; GetModuleFileNameW(0,me,MAX_PATH); DWORD h; GetFileVersionInfoSizeW(me,&h); char b[256]="(none)"; GetEnvironmentVariableA("VK_LOADER_DRIVERS_DISABLE",b,256); printf("Win32 env VK=%s\n", b); return 0; }
