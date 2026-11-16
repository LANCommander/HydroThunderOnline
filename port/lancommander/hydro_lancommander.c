/*
 * hydro_lancommander.dll: the LANCommander Interposer, embedded (LANCommander.Interposer.Embedded.h) with only its
 * match statistics feature. hydro.exe reports races through it (host/lc_stats.c); see port/CMakeLists.txt for why
 * it is a DLL of its own.
 *
 * It behaves as the Interposer deployed under this file name: its base directory is this DLL's (hydro.exe's), so
 * its configuration is .interposer\Config.yml there and its session log goes to .interposer\Logs\. It installs no
 * hooks (InterposerEmbeddedAttachApiOnly): only the API, the config, the log and the launcher's event channel. If
 * another Interposer is already in the process (LANCommander can deploy its own as version.dll), attaching
 * installs nothing and every Interposer* call forwards to that one.
 */
#include <windows.h>

#include <LANCommander.Interposer.Embedded.h>

static HMODULE g_module;
static BOOL g_attached;

/* Once hydro.exe has loaded the DLL, not from DllMain: attaching reads the config and starts the delivery thread. */
__declspec(dllexport) void __cdecl HydroLancommanderAttach(void)
{
    if (!g_attached) {
        InterposerEmbeddedAttachApiOnly(g_module);
        g_attached = TRUE;
    }
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved)
{
    if (reason == DLL_PROCESS_ATTACH) {
        g_module = instance;
        DisableThreadLibraryCalls(instance);
    } else if (reason == DLL_PROCESS_DETACH && g_attached) {
        InterposerEmbeddedDetach(reserved != NULL);
    }
    return TRUE;
}
