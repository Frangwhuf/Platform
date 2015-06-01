#pragma once

#include <tools/Interface.h>
#include <tools/InterfaceTools.h>
#include <tools/Timing.h>

namespace tools {
   struct TimerQueue
       : Disposable
   {
       virtual AutoDispose< Request > timer( uint64, uint64 *, void * = TOOLS_RETURN_ADDRESS() ) = 0;
       virtual uint64 eval( uint64 * = nullptr ) = 0;
   };

   namespace impl {
       uint64 getHighResTime( void );
       AutoDispose< TimerQueue > timerQueueNew( Thunk const & );
       void annotateThread( StringId const & );
   };  // impl namespace
};  // tools namespace
