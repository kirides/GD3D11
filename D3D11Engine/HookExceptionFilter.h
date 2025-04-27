#pragma once
#if _MSC_VER >= 1300
#include <Tlhelp32.h>
#endif

#include "pch.h"

#include "StackWalker.h"

// Simple implementation of an additional output to the console:
class MyStackWalker : public StackWalker {
public:
    MyStackWalker() : StackWalker() { LoadModules(); }
    MyStackWalker( DWORD dwProcessId, HANDLE hProcess ) : StackWalker( dwProcessId, hProcess ) {}
    virtual void OnOutput( LPCSTR szText ) { Log( "STACK", __FILE__, __LINE__, __FUNCSIG__ ) << szText; StackWalker::OnOutput( szText ); }

    static MyStackWalker& GetSingleton() { static MyStackWalker singleton; return singleton; }
};

// The exception filter function:
static LONG WINAPI ExpFilter( EXCEPTION_POINTERS* pExp, DWORD dwExpCode ) {
    // Print callstack
    MyStackWalker::GetSingleton().ShowCallstack( GetCurrentThread(), pExp->ContextRecord );

    // Show message:
    /*MessageBoxA(nullptr, "GD3D11 crashed due to internal problems. A detailed description can be found in system\\log.txt.\n\n"
        "Be sure to include this File if you want to report the crash in the Forums!", "GD3D11 has encountered a problem and can not continue.", MB_OK | MB_ICONERROR);

    exit(0);*/

    return EXCEPTION_EXECUTE_HANDLER;
}

//#define RESET_STACK {	BYTE* pStack; 	BYTE* pBase; __asm{	mov pStack, esp} __asm{mov pBase, ebp} for(; pStack < pBase && *pStack != 0x0001003f; pStack++);	CONTEXT* context = (CONTEXT *)pStack;MyStackWalker sw;	sw.ShowCallstack(GetCurrentThread(), context); }

__declspec(selectany) std::vector<std::string> _functions;
static void __AddDbgFuncCall( const std::string& fn, int threadID, bool out ) {
    std::string o;
    if ( out )
        o = "OUT - ";
    else
        o = "IN - ";

    _functions.push_back( o + std::to_string( threadID ) + ": " + fn );
}

#ifdef PUBLIC_RELEASE
#define hook_infunc __try {

#define hook_outfunc } __except (ExpFilter(GetExceptionInformation(), GetExceptionCode())){}
#else
#define hook_infunc  //__AddDbgFuncCall(__FUNCTION__, GetCurrentThreadId(), true);

#define hook_outfunc //__AddDbgFuncCall(__FUNCTION__, GetCurrentThreadId(), false);
#endif


/*
#define hook_outfunc      } catch (...) { \
                                                LogInfo() << "Exception caught!"; \
                                                                    \
                                                }
                                                */
