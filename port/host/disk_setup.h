#pragma once
/*
 * Game data from the arcade hard disk (MAME's hydro.chd). The port ships no game files: the first
 * run finds the CHD (or asks for it) and extracts its FAT16 volumes to <data>\C, D and E.
 */

/* Make sure g_cfg.data_root holds the extracted disk and g_cfg.exe_path exists. chd is the
 * --chd argument (always extract from it) or NULL (extract only if the data is missing). */
void disk_setup(const char *chd);
