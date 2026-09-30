#include "HookExceptionFilter.h"
#include <TlHelp32.h>
#include "StackWalker.h"

namespace {
    class MyStackWalker : public StackWalker {
    public:
        void OnOutput( LPCSTR szText ) override {
            std::string_view text( szText );
            while ( !text.empty() && (text.back() == '\n' || text.back() == '\r') ) text.remove_suffix( 1 );
            Logging::Inf( "STACK: {}", text );
            StackWalker::OnOutput( szText );
        }

        static MyStackWalker& GetSingleton() { static MyStackWalker singleton; return singleton; }
    };
}

LONG WINAPI ExpFilter( EXCEPTION_POINTERS* pExp, DWORD dwExpCode ) {
    MyStackWalker::GetSingleton().ShowCallstack( GetCurrentThread(), pExp->ContextRecord );
    Logging::Flush();
    return EXCEPTION_EXECUTE_HANDLER;
}

void InitCrashStackWalker() {
    MyStackWalker::GetSingleton();
}
