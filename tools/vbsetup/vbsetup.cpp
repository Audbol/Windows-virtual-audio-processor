// vbsetup - installs / removes the root-enumerated "VocalBridge Virtual Mic" device.
//
// A virtual audio adapter has no bus to enumerate it, so (like `devcon install`)
// we create a ROOT\VocalBridge device node and point it at VocalBridge.inf.
//
//   vbsetup install   <path\to\VocalBridge.inf>
//   vbsetup update    <path\to\VocalBridge.inf>
//   vbsetup uninstall
//   vbsetup status
//
// Must run elevated (the PowerShell scripts in /scripts take care of that).

#ifndef UNICODE
 #define UNICODE
#endif
#ifndef _UNICODE
 #define _UNICODE
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <setupapi.h>
#include <newdev.h>
#include <devguid.h>
#include <cfgmgr32.h>
#include <cstdio>
#include <cwchar>
#include <string>
#include <vector>

#ifdef _MSC_VER
 #pragma comment(lib, "setupapi.lib")
 #pragma comment(lib, "newdev.lib")
#endif

static const wchar_t kHardwareId[] = L"ROOT\\VocalBridge";

static void printError (const wchar_t* what)
{
    const DWORD err = GetLastError();
    wchar_t* msg = nullptr;
    FormatMessageW (FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                    nullptr, err, 0, (LPWSTR) &msg, 0, nullptr);
    fwprintf (stderr, L"error: %ls failed (0x%08lx) %ls\n", what, err, msg != nullptr ? msg : L"");
    if (msg != nullptr)
        LocalFree (msg);
}

static bool hasOurHardwareId (HDEVINFO set, SP_DEVINFO_DATA& info)
{
    DWORD type = 0, size = 0;
    SetupDiGetDeviceRegistryPropertyW (set, &info, SPDRP_HARDWAREID, &type, nullptr, 0, &size);
    if (size == 0)
        return false;

    std::vector<wchar_t> ids (size / sizeof (wchar_t) + 2, L'\0');
    if (! SetupDiGetDeviceRegistryPropertyW (set, &info, SPDRP_HARDWAREID, &type, (PBYTE) ids.data(), size, nullptr))
        return false;

    for (const wchar_t* id = ids.data(); *id != L'\0'; id += wcslen (id) + 1)
        if (_wcsicmp (id, kHardwareId) == 0)
            return true;

    return false;
}

/** Calls fn(set, info) for every present-or-not device with our hardware id. Returns count. */
template <typename Fn>
static int forEachOurDevice (Fn&& fn)
{
    HDEVINFO set = SetupDiGetClassDevsW (nullptr, nullptr, nullptr, DIGCF_ALLCLASSES);
    if (set == INVALID_HANDLE_VALUE)
        return 0;

    int count = 0;
    SP_DEVINFO_DATA info {};
    info.cbSize = sizeof (SP_DEVINFO_DATA);
    for (DWORD i = 0; SetupDiEnumDeviceInfo (set, i, &info); ++i)
    {
        if (hasOurHardwareId (set, info))
        {
            ++count;
            fn (set, info);
        }
    }

    SetupDiDestroyDeviceInfoList (set);
    return count;
}

static std::wstring fullPath (const wchar_t* path)
{
    wchar_t buffer[MAX_PATH * 2] {};
    const DWORD len = GetFullPathNameW (path, (DWORD) (sizeof (buffer) / sizeof (buffer[0])), buffer, nullptr);
    return (len > 0) ? std::wstring (buffer) : std::wstring (path);
}

static int updateDriver (const std::wstring& inf)
{
    BOOL reboot = FALSE;
    if (! UpdateDriverForPlugAndPlayDevicesW (nullptr, kHardwareId, inf.c_str(), INSTALLFLAG_FORCE, &reboot))
    {
        printError (L"UpdateDriverForPlugAndPlayDevices");
        return 1;
    }

    wprintf (L"VocalBridge driver installed%ls\n", reboot ? L" - please reboot to finish." : L".");
    return 0;
}

static int install (const wchar_t* infArg)
{
    const std::wstring inf = fullPath (infArg);

    if (GetFileAttributesW (inf.c_str()) == INVALID_FILE_ATTRIBUTES)
    {
        fwprintf (stderr, L"error: INF not found: %ls\n", inf.c_str());
        return 1;
    }

    if (forEachOurDevice ([] (HDEVINFO, SP_DEVINFO_DATA&) {}) > 0)
    {
        wprintf (L"Device %ls already exists, updating its driver...\n", kHardwareId);
        return updateDriver (inf);
    }

    GUID classGuid {};
    wchar_t className[MAX_CLASS_NAME_LEN] {};
    if (! SetupDiGetINFClassW (inf.c_str(), &classGuid, className, MAX_CLASS_NAME_LEN, nullptr))
    {
        printError (L"SetupDiGetINFClass");
        return 1;
    }

    HDEVINFO set = SetupDiCreateDeviceInfoList (&classGuid, nullptr);
    if (set == INVALID_HANDLE_VALUE)
    {
        printError (L"SetupDiCreateDeviceInfoList");
        return 1;
    }

    int result = 1;
    SP_DEVINFO_DATA info {};
    info.cbSize = sizeof (SP_DEVINFO_DATA);

    // REG_MULTI_SZ: "ROOT\VocalBridge\0\0"
    wchar_t hardwareIds[64] {};
    wcscpy_s (hardwareIds, sizeof (hardwareIds) / sizeof (hardwareIds[0]), kHardwareId);
    const DWORD hardwareIdsBytes = (DWORD) ((wcslen (kHardwareId) + 2) * sizeof (wchar_t));

    if (! SetupDiCreateDeviceInfoW (set, className, &classGuid, nullptr, nullptr, DICD_GENERATE_ID, &info))
        printError (L"SetupDiCreateDeviceInfo");
    else if (! SetupDiSetDeviceRegistryPropertyW (set, &info, SPDRP_HARDWAREID, (const BYTE*) hardwareIds, hardwareIdsBytes))
        printError (L"SetupDiSetDeviceRegistryProperty");
    else if (! SetupDiCallClassInstaller (DIF_REGISTERDEVICE, set, &info))
        printError (L"SetupDiCallClassInstaller(DIF_REGISTERDEVICE)");
    else
    {
        result = updateDriver (inf);

        if (result != 0)   // don't leave a driverless phantom device behind
            SetupDiCallClassInstaller (DIF_REMOVE, set, &info);
    }

    SetupDiDestroyDeviceInfoList (set);
    return result;
}

static int uninstall()
{
    int failures = 0;
    const int count = forEachOurDevice ([&failures] (HDEVINFO set, SP_DEVINFO_DATA& info)
    {
        if (! SetupDiCallClassInstaller (DIF_REMOVE, set, &info))
        {
            printError (L"SetupDiCallClassInstaller(DIF_REMOVE)");
            ++failures;
        }
    });

    if (count == 0)
        wprintf (L"No VocalBridge device found.\n");
    else if (failures == 0)
        wprintf (L"Removed %d VocalBridge device(s).\n", count);

    return failures == 0 ? 0 : 1;
}

static int status()
{
    const int count = forEachOurDevice ([] (HDEVINFO set, SP_DEVINFO_DATA& info)
    {
        ULONG devStatus = 0, problem = 0;
        const CONFIGRET cr = CM_Get_DevNode_Status (&devStatus, &problem, info.DevInst, 0);
        wchar_t name[256] = L"(unnamed)";
        SetupDiGetDeviceRegistryPropertyW (set, &info, SPDRP_DEVICEDESC, nullptr, (PBYTE) name, sizeof (name), nullptr);

        if (cr != CR_SUCCESS)
            wprintf (L"%ls: not present\n", name);
        else if (devStatus & DN_HAS_PROBLEM)
            wprintf (L"%ls: PROBLEM code %lu (52 = signature not trusted: enable test signing / install the certificate)\n", name, problem);
        else
            wprintf (L"%ls: %ls\n", name, (devStatus & DN_STARTED) ? L"running" : L"installed, not started");
    });

    if (count == 0)
        wprintf (L"VocalBridge is not installed.\n");

    HANDLE h = CreateFileW (L"\\\\.\\VocalBridge", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    wprintf (L"App link (\\\\.\\VocalBridge): %ls\n", h != INVALID_HANDLE_VALUE ? L"available" : L"not available");
    if (h != INVALID_HANDLE_VALUE)
        CloseHandle (h);

    return count > 0 ? 0 : 1;
}

int wmain (int argc, wchar_t** argv)
{
    if (argc >= 3 && _wcsicmp (argv[1], L"install") == 0)   return install (argv[2]);
    if (argc >= 3 && _wcsicmp (argv[1], L"update") == 0)    return updateDriver (fullPath (argv[2]));
    if (argc >= 2 && _wcsicmp (argv[1], L"uninstall") == 0) return uninstall();
    if (argc >= 2 && _wcsicmp (argv[1], L"status") == 0)    return status();

    fwprintf (stderr,
              L"usage:\n"
              L"  vbsetup install   <path\\to\\VocalBridge.inf>\n"
              L"  vbsetup update    <path\\to\\VocalBridge.inf>\n"
              L"  vbsetup uninstall\n"
              L"  vbsetup status\n");
    return 2;
}
