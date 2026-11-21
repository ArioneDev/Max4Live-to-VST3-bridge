#pragma once

#include <juce_core/juce_core.h>

#include <windows.h>
#include <tlhelp32.h>

namespace m4lb
{

inline juce::File moduleFile()
{
    HMODULE handle = nullptr;
    const auto addr = reinterpret_cast<LPCWSTR> (reinterpret_cast<const void*> (&moduleFile));

    if (! GetModuleHandleExW (GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                                  | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                              addr, &handle))
        return {};

    wchar_t buffer[32768] = {};
    const auto len = GetModuleFileNameW (handle, buffer, 32768);

    if (len == 0)
        return {};

    return juce::File (juce::String (buffer, (int) len));
}

// Folder that contains the .vst3 bundles (…/VST3).
inline juce::File plugInFolder()
{
    auto f = moduleFile(); // …/X.vst3/Contents/x86_64-win/X.vst3
    return f.getParentDirectory()      // x86_64-win
            .getParentDirectory()      // Contents
            .getParentDirectory()      // X.vst3
            .getParentDirectory();     // folder holding the bundles
}

// Everywhere the shims could reasonably live.  Order matters: the first
// existing directory wins.
inline juce::Array<juce::File> shimCandidates (const juce::String& bundleName)
{
    juce::Array<juce::File> candidates;

    const auto module = moduleFile();
    const auto bundle = module.getParentDirectory().getParentDirectory().getParentDirectory();
    const auto folder = bundle.getParentDirectory();

    candidates.add (folder.getChildFile (bundleName));                  // sibling bundle
    candidates.add (bundle.getChildFile (bundleName));                  // copied inside
    candidates.add (module.getParentDirectory().getChildFile (bundleName)); // next to the dll
    candidates.add (folder.getParentDirectory().getChildFile (bundleName));

    if (auto env = juce::SystemStats::getEnvironmentVariable ("M4LBRIDGE_SHIM_DIR", {}); env.isNotEmpty())
        candidates.add (juce::File (env).getChildFile (bundleName));

    candidates.add (juce::File ("C:/Program Files/Common Files/VST3").getChildFile (bundleName));
    candidates.add (juce::File ("C:/Program Files (x86)/Common Files/VST3").getChildFile (bundleName));

    return candidates;
}

inline juce::File findShimBundle (const juce::String& bundleName)
{
    // Accept a bundle directory or, if the user unpacked the zip by hand and
    // ended up with a bare DLL, that file as well.
    for (const auto& c : shimCandidates (bundleName))
        if (c.isDirectory() || c.existsAsFile())
            return c;

    return {};
}

inline juce::String readRegString (HKEY root, const wchar_t* subKey, const wchar_t* valueName)
{
    HKEY key = nullptr;
    if (RegOpenKeyExW (root, subKey, 0, KEY_READ, &key) != ERROR_SUCCESS)
        return {};

    wchar_t buffer[1024] = {};
    DWORD size = sizeof (buffer);
    DWORD type = 0;
    juce::String result;

    if (RegQueryValueExW (key, valueName, nullptr, &type, reinterpret_cast<LPBYTE> (buffer), &size) == ERROR_SUCCESS
        && (type == REG_SZ || type == REG_EXPAND_SZ))
        result = juce::String (buffer);

    RegCloseKey (key);
    return result;
}

inline juce::File findMaxExecutable (const juce::String& overridePath);

inline bool isProcessRunning (const juce::String& exeName)
{
    HANDLE snap = CreateToolhelp32Snapshot (TH32CS_SNAPPROCESS, 0);

    if (snap == INVALID_HANDLE_VALUE)
        return false;

    PROCESSENTRY32W entry {};
    entry.dwSize = sizeof (entry);

    bool found = false;

    if (Process32FirstW (snap, &entry))
    {
        do
        {
            if (juce::String (entry.szExeFile).equalsIgnoreCase (exeName))
            {
                found = true;
                break;
            }
        }
        while (Process32NextW (snap, &entry));
    }

    CloseHandle (snap);
    return found;
}

// Max only allows one instance per executable, but Max.exe / MaxRT.exe /
// MaxRT_nocef.exe can run side by side.  Pick one that is not running yet so
// several bridge instances can each own a Max.
inline juce::File findFreeMaxExecutable (const juce::String& preferred = {})
{
    auto base = findMaxExecutable (preferred);

    if (! base.existsAsFile())
        return base;

    auto dir = base.getParentDirectory();

    juce::StringArray names { base.getFileName() };
    for (const auto* n : { "Max.exe", "MaxRT.exe", "MaxRT_nocef.exe" })
        names.addIfNotAlreadyThere (n);

    for (const auto& name : names)
    {
        auto candidate = dir.getChildFile (name);

        if (candidate.existsAsFile() && ! isProcessRunning (name))
            return candidate;
    }

    return base;
}

// Looks for the Max editor / runtime in the usual places, the uninstall
// registry entries, and finally a user override.
inline juce::File findMaxExecutable (const juce::String& overridePath = {})
{
    if (overridePath.isNotEmpty() && juce::File (overridePath).existsAsFile())
        return juce::File (overridePath);

    if (auto env = juce::SystemStats::getEnvironmentVariable ("M4LBRIDGE_MAX_EXE", {}); env.isNotEmpty())
        if (juce::File (env).existsAsFile())
            return juce::File (env);

    juce::Array<juce::File> candidates;

    for (const auto* base : { "C:/Program Files/Cycling '74", "C:/Program Files (x86)/Cycling '74" })
    {
        juce::File dir (base);
        if (! dir.isDirectory())
            continue;

        for (int v = 10; v >= 6; --v)
        {
            candidates.add (dir.getChildFile ("Max " + juce::String (v)).getChildFile ("Max.exe"));
            candidates.add (dir.getChildFile ("Max " + juce::String (v)).getChildFile ("MaxRT.exe"));
        }
    }

    // Uninstall entries ("Max version 9.2.0" with InstallLocation F:\Max\).
    const wchar_t* uninstallKeys[] = {
        L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall",
        L"SOFTWARE\\WOW6432Node\\Microsoft\\Windows\\CurrentVersion\\Uninstall"
    };

    for (HKEY root : { HKEY_LOCAL_MACHINE, HKEY_CURRENT_USER })
    {
        for (const wchar_t* sub : uninstallKeys)
        {
            HKEY key = nullptr;
            if (RegOpenKeyExW (root, sub, 0, KEY_READ, &key) != ERROR_SUCCESS)
                continue;

            DWORD index = 0;
            wchar_t nameBuf[512] = {};

            while (true)
            {
                DWORD nameLen = 512;
                if (RegEnumKeyExW (key, index++, nameBuf, &nameLen, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS)
                    break;

                const juce::String childKey = juce::String (sub) + "\\" + juce::String (nameBuf);
                const juce::String display = readRegString (root, childKey.toWideCharPointer(), L"DisplayName");

                if (display.startsWithIgnoreCase ("Max version") || display.equalsIgnoreCase ("Max"))
                {
                    const juce::String loc = readRegString (root, childKey.toWideCharPointer(), L"InstallLocation");
                    if (loc.isNotEmpty())
                    {
                        candidates.add (juce::File (loc).getChildFile ("Max.exe"));
                        candidates.add (juce::File (loc).getChildFile ("MaxRT.exe"));
                    }
                }
            }

            RegCloseKey (key);
        }
    }

    for (const auto& c : candidates)
        if (c.existsAsFile())
            return c;

    return {};
}

} // namespace m4lb
