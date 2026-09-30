#ifndef OTTER_OTTER_GLOBAL_H
#define OTTER_OTTER_GLOBAL_H

#include <stdcorelib/stdc_global.h>

#ifndef OTTER_EXPORT
#  ifdef OTTER_STATIC
#    define OTTER_EXPORT
#  else
#    ifdef OTTER_LIBRARY
#      define OTTER_EXPORT STDC_DECL_EXPORT
#    else
#      define OTTER_EXPORT STDC_DECL_IMPORT
#    endif
#  endif
#endif

#endif // OTTER_OTTER_GLOBAL_H
