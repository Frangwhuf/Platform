#include "../toolsprecompiled.h"

#include <tools/Interface.h>
#include <tools/Memory.h>
#include <tools/String.h>
#include <tools/Tools.h>

#include <dlfcn.h> // dladdr
#include <cxxabi.h>  // demangling
#include <execinfo.h>  // backtrace and friends
#include <signal.h>
#include <ucontext.h>
#include <algorithm>
#include <memory>

using namespace tools;

#define TOOLS_TIME_LEN ( CTIME_LEN + 4 )  // add 4 for millisecs, preceded by dot

StringId
gccDemangle(
    char const * name )
{
    int status;
    std::unique_ptr< char > demangledName( abi::__cxa_demangle( name, nullptr, nullptr, &status ) );
    StringId sidName;
    if( status == 0 && static_cast< bool >( demangledName )) {
        sidName = StringId( demangledName.get() );
    } else {
        sidName = StringId( name );
    }
    return sidName;
}

StringId
detail::platformDemangleTypeInfo(
    std::type_info const & typ ) throw()
{
    char const * name = typ.name();
    return gccDemangle( name );
}

StringId
detail::platformDemangleSymbol(
    StringId const & s ) throw()
{
    return gccDemangle( s.c_str() );
}

StringId
detail::platformSymbolNameFromAddress(
    void * site,
    unsigned * offset )
{
    Dl_info info;
    if( dladdr( site, &info ) && !!info.dli_sname ) {
        if( !!offset ) {
            *offset = reinterpret_cast< uintptr_t >( site ) - reinterpret_cast< uintptr_t >( info.dli_saddr );
        }
        return info.dli_sname;
    }
    if( !!offset ) {
        *offset = 0;
    }
#if (__SIZEOF_POINTER__ == 8)
    char a[19];  // 16 hex digits + '0x' + nullptr
#else // 64-bit test
    char a[11];  // 8 hex digits + '0x' + nullptr
#endif // 64-bit test
    snprintf( a, sizeof( a ), "%p", site );
    return a;
}

namespace {
    char *
    getTimestampString(
        char suppliedBuf[ 26 ] )  // according to man page, 26 characters for ctime_r
    {
        // This should, in principal be faster than walltimeNsec(), since that is doing work this would
        // have to undo. None of the below emits an integer divide.
        struct timespec tod;
        clock_gettime( CLOCK_REALTIME, &tod );
        time_t clock = tod.tv_sec;
        unsigned msecsRemainder = tod.tv_nsec / 1000 / 1000;
        TOOLS_ASSERT( msecsRemainder < 1000 );
        char * timestamp = ctime_r( &clock, suppliedBuf ) + CTIME_SKIP;
        timestamp[ CTIME_LEN ] = '.';
        timestamp[ CTIME_LEN + 1 ] = '0' + ( msecsRemainder / 100 );
        msecsRemainder %= 100;
        timestamp[ CTIME_LEN + 2 ] = '0' + ( msecsRemainder / 10 );
        msecsRemainder %= 10;
        timestamp[ CTIME_LEN + 3 ] = '0' + msecsRemainder;
        timestamp[ CTIME_LEN + 4 ] = 0;
        static_assert( TOOLS_TIME_LEN + CTIME_SKIP + 1 <= 26, "bad CTIME_LEN" );
        return timestamp;
    }

    char *
    backtraceDemangle(
        char const * inBuf )
    {
        if( !inBuf ) {
            return nullptr;
        }
        unsigned const extraSpace = 200;
        char * outBuf = static_cast< char * >( impl::safeMalloc( strlen( inBuf ) + extraSpace + 1 ));
        if( !outBuf ) {
            return nullptr;
        }
        char * outP = outBuf;
        char const * inP = inBuf;
        char * mangledHere = nullptr;
        // Copy the string past the '(' until we hit '+'
        while( *inP ) {
            *outP = *inP;
            if( *inP == '(' ) {
                mangledHere = outP + 1;
            }
            if(( *inP == '+' ) && !!mangledHere ) {
                *outP = '\0';
                break;
            }
            ++inP;
            ++outP;
        }
        // Try to demangle the symbol
        if( *inP && !!mangledHere ) {
            int status = -1;
            char * demangled = abi::__cxa_demangle( mangledHere, nullptr, nullptr, &status );
            if( status == 0 ) {
                int mangledLen = strlen( mangledHere );
                int demangledLen = strlen( demangled );
                strncpy( mangledHere, demangled, extraSpace + mangledLen - 1 );
                free( demangled );
                outP = std::min( mangledHere + demangledLen, mangledHere + extraSpace + mangledLen - 1 );
            } else {
                // demangling failure
                strcpy( outBuf, inBuf );
                return outBuf;
            }
        }
        // Copy the rest
        while( *inP ) {
            *outP++ = *inP++;
        }
        *outP = '\0';
        return outBuf;
    }

    bool
    localPrintStack(
        FILE * output,
        int maxDepth )
    {
        void * bt[ maxDepth ];  // yes, variable sized array, thank you C++11!
        char ** strBt;
        int depth = backtrace( bt, maxDepth );
        strBt = backtrace_symbols( bt, depth );
        if( !strBt ) {
            return false;
        }
        for( int i=0; i<depth; ++i ) {
            char * demangled = backtraceDemangle( strBt[ i ]);
            if( !demangled ) {
                fprintf( output, "STACK TRACE %d >> %s\n", i, strBt[ i ]);
            } else {
                fprintf( output, "STACK TRACE %d >> %s\n", i, demangled );
                free( demangled );
            }
        }
        free( strBt );
        if( depth == maxDepth ) {
            fprintf( output, "STACK TRAcE >> ...\n" );
        }
        return true;
    }

    // Dump registers, stack trace, and stack dump.
    void
    dumpStackTrace(
        bool includeHeader,
        bool includeRegisters,
        FILE * output,
        int sig,
        siginfo_t * sip,
        void * addr,
        bool externallySent )
    {
        char timestampBuffer[ 26 ];
        if( includeHeader ) {
            fprintf( output, "%s [BEGIN STACK TRACE]\n", getTimestampString( timestampBuffer ));
        }
        if( sig != 0 ) {
            fprintf( output, "STACK TRACE >> Signal %d: '%s' caught on pid %d, thread ID %lX\n",
                sig, strsignal( sig ), static_cast< int >( getpid() ), impl::threadId() );
            fprintf( output, "STACK TRACE >> signo=%d, sigcode (reason sent)=%d, external=%u\n",
                sip->si_signo, sip->si_code, static_cast< unsigned >( externallySent ));
            if( ( sig == SIGILL ) || ( sig == SIGFPE ) || ( sig == SIGSEGV ) || ( sig == SIGBUS ) || ( sig == SIGTRAP )) {
                fprintf( output, "STACK TRACE >> fault address=0x%lx\n", reinterpret_cast< uintptr_t >( sip->si_addr ));
            }
            // Print general purpose registers. GDB can't capture the precise register state at the time
            // of signal because this function has already changed it significantly. Often this isn't a
            // problem for core file analysis, but occationally it is. The ucontext_t contains the true
            // state at the time of the signal and therefore is worth printing.
            const ucontext_t * context = static_cast< ucontext_t * >( addr );
            const struct sigcontext * sc = (struct sigcontext *)&context->uc_mcontext;
            fprintf( output, "STACK TRACE >> rip=0x%lx, rflags=0x%x, trapno=0x%x",
                sc->rip, static_cast< uint32 >( sc->eflags ), static_cast< uint32 >( sc->trapno ));
            if( sc->trapno == 14 ) {  // page fault
                fprintf( output, ", cr2=0x%lx", sc->cr2 );
            }
            fprintf( output, "\nSTACK TRACE >> rax=0x%lx, rbx=0x%lx, rcx=0x%lx, rdx=0x%lx\n"
                "STACK TRACE >> rsi=0x%lx, rdi=0x%lx, rbp=0x%lx, rsp=0x%lx\n"
                "STACK TRACE >> r8=0x%lx, r9=0x%lx, r10=0x%lx, r11=0x%lx\n"
                "STACK TRACE >> r12=0x%lx, r13=0x%lx, r14=0x%lx, r15=0x%lx\n",
                sc->rax, sc->rbx, sc->rcx, sc->rdx, sc->rsi, sc->rdi, sc->rbp, sc->rsp,
                sc->r8, sc->r9, sc->r10, sc->r11, sc->r12, sc->r13, sc->r14, sc->r15 );
        }
        localPrintStack( output, 5000 );
        if( includeHeader ) {
            fprintf( output, "%s [END STACK TRACE]\n", getTimestampString( timestampBuffer ));
            fprintf( output, "%s [BEGIN RAW STACK DUMP]\n", getTimestampString( timestampBuffer ));
        }
        uint64 lRsp;
        asm volatile ( "movq %%rsp, %0" : "=r"(lRsp) : : );
        // Round %rsp down to 0x10 for simplicity of display. The entire page will be mapped regardless.
        const uint64 * rsp = reinterpret_cast< uint64 * >( roundDownPow2( lRsp, 0x10 ));
        const uint64 * stackTop = reinterpret_cast< uint64 * >( stackTopAddr );
        if( !stackTop ) {
            // Don't know the top of the stack. The top of the page is the only safe place we can look.
            stackTop = reinterpret_cast< uint64 * >( roundUpPow2( lRsp, 0x1000 ));
        }
        // Limit the maximum stack size to display to 64 KB
        const size_t maxStackWords = 0x10000 / sizeof( *stackTop );
        if( static_cast< uint64 >( stackTop - rsp ) > maxStackWords ) {
            stackTop = rsp + maxStackWords;
        }
        const uint64 * pos = rsp;
        while( pos + 0x2 <= stackTop ) {
            fprintf( output, "%p: %016lx %016lx\n", pos, pos[0], pos[1] );
            pos += 0x2;
        }
        if( includeHeader ) {
            fprintf( output, "%s [END RAW STACK DUMP]\n", getTimestampString( timestampBuffer ));
        }
        fflush( output );
    }
};  // anonymous namespace

void
detail::logStackTrace(
    bool includeHeader,
    bool includeRegisters )
{
    dumpStackTrace( includeHeader, includeRegisters, stdout, 0, nullptr, nullptr, false );
}