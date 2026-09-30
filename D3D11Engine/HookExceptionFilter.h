#pragma once

/** Logs the faulting callstack and lets the enclosing __except handle the exception. */
LONG WINAPI ExpFilter( EXCEPTION_POINTERS* pExp, DWORD dwExpCode );

/** Loads the stack walker's module list up front, so a crash doesn't have to. */
void InitCrashStackWalker();

#ifdef PUBLIC_RELEASE
#define hook_infunc __try {

#define hook_outfunc } __except (ExpFilter(GetExceptionInformation(), GetExceptionCode())){}
#else
#define hook_infunc
#define hook_outfunc
#endif
