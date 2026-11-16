"""Checks that executables load on Windows XP SP3 and need no runtime DLLs (CI; see port/CMakeLists.txt).

    python check_imports.py hydro.exe htmaster.exe

For each executable:
- the PE header's OS and subsystem versions are at most 5.1 (XP);
- it imports only from system DLLs that XP has (so not the VC++ or universal CRT DLLs: the CRT is static);
- it imports none of the functions below, which those DLLs only gained after XP. The list covers what the CRT,
  SDL and code like the host's are likely to reach for; port/common/winxp_crt.c stands in for the CRT's.
Requires pefile (pip install pefile).
"""
import sys

import pefile

XP_DLLS = {
    "advapi32.dll", "comdlg32.dll", "dnsapi.dll", "gdi32.dll", "imm32.dll", "iphlpapi.dll", "kernel32.dll",
    "ole32.dll", "oleaut32.dll", "setupapi.dll", "shell32.dll", "user32.dll", "version.dll", "winmm.dll",
    "ws2_32.dll",
}

# Functions of the DLLs above that are Vista or later.
NOT_ON_XP = {
    "kernel32.dll": """
        AcquireSRWLockExclusive AcquireSRWLockShared AddDllDirectory CancelIoEx CancelSynchronousIo
        CloseThreadpoolTimer CloseThreadpoolWait CloseThreadpoolWork CompareStringEx CompareStringOrdinal CopyFile2
        CreateEventExA CreateEventExW CreateFile2 CreateMutexExA CreateMutexExW CreateRemoteThreadEx
        CreateSemaphoreExA CreateSemaphoreExW CreateSymbolicLinkA CreateSymbolicLinkW CreateThreadpoolTimer
        CreateThreadpoolWait CreateThreadpoolWork CreateWaitableTimerExA CreateWaitableTimerExW
        DeleteProcThreadAttributeList EnumSystemLocalesEx FindFirstFileNameW FindFirstStreamW FlsAlloc FlsFree
        FlsGetValue FlsGetValue2 FlsSetValue FlsSetValue2 FlushProcessWriteBuffers FreeLibraryWhenCallbackReturns
        GetCalendarInfoEx GetCurrencyFormatEx GetCurrentPackageId GetCurrentProcessorNumber
        GetCurrentProcessorNumberEx GetDateFormatEx GetDynamicTimeZoneInformation GetErrorMode
        GetFileInformationByHandleEx GetFinalPathNameByHandleA GetFinalPathNameByHandleW GetLocaleInfoEx
        GetLogicalProcessorInformationEx GetNumberFormatEx GetOverlappedResultEx GetProductInfo
        GetQueuedCompletionStatusEx GetSystemDefaultLocaleName GetSystemTimePreciseAsFileTime GetThreadDescription
        GetThreadErrorMode GetThreadId GetTickCount64 GetTimeFormatEx GetUserDefaultLocaleName
        GetVolumeInformationByHandleW InitOnceBeginInitialize InitOnceComplete InitOnceExecuteOnce
        InitOnceInitialize InitializeConditionVariable InitializeCriticalSectionEx InitializeProcThreadAttributeList
        InitializeSRWLock InterlockedCompareExchange64 IsValidLocaleName LCIDToLocaleName LCMapStringEx
        LocaleNameToLCID OpenFileById QueryFullProcessImageNameA QueryFullProcessImageNameW
        QueryUnbiasedInterruptTime RaiseFailFastException ReOpenFile RegisterApplicationRestart ReleaseSRWLockExclusive
        ReleaseSRWLockShared RemoveDllDirectory ResolveLocaleName SetDefaultDllDirectories
        SetFileCompletionNotificationModes SetFileInformationByHandle SetThreadDescription SetThreadErrorMode SetThreadGroupAffinity
        SetThreadStackGuarantee SetThreadpoolTimer SetThreadpoolWait SleepConditionVariableCS
        SleepConditionVariableSRW SubmitThreadpoolWork TryAcquireSRWLockExclusive TryAcquireSRWLockShared
        TrySubmitThreadpoolCallback UpdateProcThreadAttribute WaitForThreadpoolTimerCallbacks
        WaitForThreadpoolWorkCallbacks WakeAllConditionVariable WakeConditionVariable Wow64DisableWow64FsRedirection
        Wow64RevertWow64FsRedirection
    """,
    "user32.dll": """
        AddClipboardFormatListener AdjustWindowRectExForDpi CalculatePopupWindowPosition ChangeWindowMessageFilter
        ChangeWindowMessageFilterEx CloseTouchInputHandle DisplayConfigGetDeviceInfo EnableMouseInPointer
        GetDisplayConfigBufferSizes GetDpiForSystem GetDpiForWindow GetGestureInfo GetPointerInfo GetPointerType
        GetSystemMetricsForDpi GetTouchInputInfo GetWindowDisplayAffinity IsProcessDPIAware IsTouchWindow
        LogicalToPhysicalPoint PhysicalToLogicalPoint QueryDisplayConfig RegisterPointerInputTarget
        RegisterPowerSettingNotification RegisterTouchWindow RemoveClipboardFormatListener SetGestureConfig
        SetProcessDPIAware SetProcessDpiAwarenessContext SetThreadDpiAwarenessContext SetWindowDisplayAffinity
        ShutdownBlockReasonCreate ShutdownBlockReasonDestroy UnregisterPowerSettingNotification
    """,
    "gdi32.dll": """
        D3DKMTCloseAdapter D3DKMTOpenAdapterFromGdiDisplayName D3DKMTOpenAdapterFromHdc D3DKMTQueryAdapterInfo
        D3DKMTWaitForVerticalBlankEvent
    """,
    "advapi32.dll": """
        EventActivityIdControl EventEnabled EventProviderEnabled EventRegister EventUnregister EventWrite
        EventWriteTransfer RegCopyTreeA RegCopyTreeW RegDeleteKeyExA RegDeleteKeyExW RegDeleteKeyValueA
        RegDeleteKeyValueW RegDeleteTreeA RegDeleteTreeW RegGetValueA RegGetValueW RegLoadMUIStringA
        RegLoadMUIStringW RegSetKeyValueA RegSetKeyValueW
    """,
    "shell32.dll": """
        SHCreateItemFromParsingName SHCreateItemInKnownFolder SHCreateShellItemArrayFromIDLists
        SHEvaluateSystemCommandTemplate SHGetIDListFromObject SHGetKnownFolderIDList SHGetKnownFolderPath
        SHGetPropertyStoreForWindow SHGetStockIconInfo SHOpenWithDialog SHQueryUserNotificationState
        SHSetKnownFolderPath SetCurrentProcessExplicitAppUserModelID
    """,
    "ole32.dll": "CoGetApartmentType CoWaitForMultipleObjects",
    "ws2_32.dll": """
        FreeAddrInfoEx FreeAddrInfoExW GetAddrInfoExA GetAddrInfoExCancel GetAddrInfoExOverlappedResult
        GetAddrInfoExW InetNtopA InetNtopW InetPtonA InetPtonW SetAddrInfoExA SetAddrInfoExW WSAConnectByList
        WSAConnectByNameA WSAConnectByNameW WSAPoll WSASendMsg inet_ntop inet_pton
    """,
    "iphlpapi.dll": """
        CancelMibChangeNotify2 ConvertInterfaceIndexToLuid ConvertInterfaceLuidToIndex ConvertInterfaceLuidToNameA
        ConvertInterfaceLuidToNameW ConvertLengthToIpv4Mask FreeMibTable GetBestRoute2 GetIfEntry2 GetIfStackTable
        GetIfTable2 GetIpForwardTable2 GetIpInterfaceEntry GetIpNetTable2 GetUnicastIpAddressTable
        NotifyIpInterfaceChange NotifyRouteChange2 NotifyUnicastIpAddressChange ResolveIpNetEntry2
        if_indextoname if_nametoindex
    """,
    "dnsapi.dll": "DnsCancelQuery DnsQueryEx",
    "setupapi.dll": """
        CM_Get_DevNode_PropertyW CM_Get_Device_Interface_PropertyW CM_Register_Notification
        CM_Unregister_Notification SetupDiGetClassPropertyW SetupDiGetDeviceInterfacePropertyW
        SetupDiGetDevicePropertyW
    """,
    "version.dll": "GetFileVersionInfoExA GetFileVersionInfoExW GetFileVersionInfoSizeExA GetFileVersionInfoSizeExW",
}
NOT_ON_XP = {dll: set(names.split()) for dll, names in NOT_ON_XP.items()}


def check(path):
    problems = []
    pe = pefile.PE(path)
    oh = pe.OPTIONAL_HEADER
    for what, major, minor in (
        ("OS", oh.MajorOperatingSystemVersion, oh.MinorOperatingSystemVersion),
        ("subsystem", oh.MajorSubsystemVersion, oh.MinorSubsystemVersion),
    ):
        if (major, minor) > (5, 1):
            problems.append(f"{what} version {major}.{minor} (XP is 5.1)")
    entries = getattr(pe, "DIRECTORY_ENTRY_IMPORT", []) + getattr(pe, "DIRECTORY_ENTRY_DELAY_IMPORT", [])
    for entry in entries:
        dll = entry.dll.decode().lower()
        if dll not in XP_DLLS:
            problems.append(f"imports {dll}")
            continue
        for imp in entry.imports:
            name = imp.name.decode() if imp.name else None
            if name in NOT_ON_XP.get(dll, ()):
                problems.append(f"imports {dll}!{name} (not on XP)")
    return problems


def main():
    failed = False
    for path in sys.argv[1:]:
        problems = check(path)
        print(f"{path}: {'OK' if not problems else 'not XP compatible'}")
        for p in problems:
            print(f"  {p}")
        failed |= bool(problems)
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
