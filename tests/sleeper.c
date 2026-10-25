#include <windows.h>
int main(){ wchar_t me[MAX_PATH]; GetModuleFileNameW(0,me,MAX_PATH); DWORD h; GetFileVersionInfoSizeW(me,&h); Sleep(9000); return 0; }
