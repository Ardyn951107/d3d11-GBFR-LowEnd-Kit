#include <windows.h>
#include <stdio.h>
int main(){ wchar_t me[MAX_PATH]; GetModuleFileNameW(0,me,MAX_PATH); DWORD h; GetFileVersionInfoSizeW(me,&h);
 HMODULE a=LoadLibraryW(L"nvapi64.dll"); HMODULE b=LoadLibraryExW(L"nvapi64.dll",0,0x800); HMODULE c=LoadLibraryW(L"user32.dll");
 printf("nvapi=%p ex=%p err=%lu user32=%p\n",a,b,GetLastError(),c); return 0; }
