#pragma once

#include <tools/String.h>
#include <tools/Tools.h>

//#include <SDKDDSVer.h>

#define NOMINMAX
#define NOGDI
#define NOSOUND
#define NOTEXTMETRIC
#define NOCOMM
#define NOKANJI
#define NOHELP
#define NOMCX
#define NOMETAFILE
#define NOIME

#include <Windows.h>
#include <Psapi.h>

#include <intrin.h>

namespace tools {
    namespace impl {
        void SetThreadName( DWORD, char const * );
    };  // impl namespace
};  // tools namespace

//struct Win32DiagState {
//        // User buffer for printing into - includes space for indent levels
//        char *                  buffer;
//        size_t                  buffer_sz;
//
//        // Current indent level
//        unsigned                indent_level;
//};
//
//
struct lk_cs;

struct Win32TlsData {
        //struct Win32DiagState diag_state;

        // Most-nested lock for this thread
        lk_cs * lkCurrent;
};
//
//
//struct Win32TlsData * win32GetTlsData();


//struct Win32ProcessGlobals {
//        CRITICAL_SECTION diag_console_cs;
//        char            diag_console_prev_level;
//        char const *    diag_console_prev_src;
//        WORD            diag_console_prev_attr;
//};
//
//
//extern struct win32_process_globals win32_globals;
