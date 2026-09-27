#pragma once
#include <windows.h>

/* Arms breakpoints for HYDRO_TRACE. Call after all hooks are installed. */
void trace_install(void);
/* Called first by the VEH; returns 1 if the exception belonged to the tracer. */
int trace_handle(EXCEPTION_POINTERS *ep);
