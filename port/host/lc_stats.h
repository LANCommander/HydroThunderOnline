#pragma once
/*
 * LANCommander match statistics (lc_stats.c): every race is reported to LANCommander as a match, through the
 * LANCommander Interposer in hydro_lancommander.dll. Without HYDRO_LANCOMMANDER (CMake option) these do nothing.
 */
#ifdef HYDRO_LANCOMMANDER
void lc_stats_init(void);     /* main(): loads hydro_lancommander.dll, if it's there */
void lc_stats_frame(void);    /* every frame, after the sim (net_link_PostWorkSend) */
void lc_stats_shutdown(void); /* before the process exits: closes a race in progress */
#else
static __inline void lc_stats_init(void) {}
static __inline void lc_stats_frame(void) {}
static __inline void lc_stats_shutdown(void) {}
#endif
