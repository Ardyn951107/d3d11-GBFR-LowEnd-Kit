#include <windows.h>
#include <stdio.h>
static LONG WINAPI gameFilter(EXCEPTION_POINTERS* e){ printf("game filter called code=%lx\n", e->ExceptionRecord->ExceptionCode); fflush(stdout); return EXCEPTION_EXECUTE_HANDLER; }
__attribute__((noinline)) void crashAV(void){ volatile int* p = (int*)0x18; *p = 1; }
__attribute__((noinline)) void crashUD(void){ __asm__ volatile("ud2"); }
int main(int argc, char** argv){
  wchar_t me[MAX_PATH]; GetModuleFileNameW(NULL, me, MAX_PATH);
  DWORD h; printf("verinfo size=%lu\n", GetFileVersionInfoSizeW(me,&h)); fflush(stdout);
  SetUnhandledExceptionFilter(gameFilter);
  Sleep(1500);
  if (argc>1 && !strcmp(argv[1],"av")) crashAV();
  if (argc>1 && !strcmp(argv[1],"ud")) crashUD();
  printf("clean exit\n"); return 0;
}
