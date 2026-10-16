#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <shlguid.h>
#include <atlbase.h>
#include <stdio.h>
#include <wchar.h>

static bool Check(bool ok, const char* what)
{
    printf("%s: %s\n", ok ? "PASS" : "FAIL", what);
    return ok;
}

int wmain()
{
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(hr)) return 1;
    bool ok = true;
    wchar_t temp[MAX_PATH]{};
    if (!GetTempPathW(MAX_PATH, temp)) return 2;
    wchar_t dir[MAX_PATH]{};
    swprintf_s(dir, L"%sOpenShell2564_%lu", temp, GetCurrentProcessId());
    CreateDirectoryW(dir, nullptr);
    wchar_t targetPath[MAX_PATH]{}, linkPath[MAX_PATH]{};
    swprintf_s(targetPath, L"%s\\real_program.exe", dir);
    swprintf_s(linkPath, L"%s\\program_shortcut.lnk", dir);
    HANDLE file = CreateFileW(targetPath, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    ok &= Check(file != INVALID_HANDLE_VALUE, "Target executable placeholder created");
    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);

    CComPtr<IShellLinkW> shortcut;
    hr = shortcut.CoCreateInstance(CLSID_ShellLink);
    ok &= Check(SUCCEEDED(hr), "IShellLink created");
    if (SUCCEEDED(hr))
    {
        hr = shortcut->SetPath(targetPath);
        ok &= Check(SUCCEEDED(hr), "Shortcut target assigned");
        CComQIPtr<IPersistFile> persist(shortcut);
        hr = persist ? persist->Save(linkPath, TRUE) : E_NOINTERFACE;
        ok &= Check(SUCCEEDED(hr), "Shortcut saved");
    }

    PIDLIST_ABSOLUTE shortcutPidl = nullptr, targetPidl = nullptr, regularPidl = nullptr;
    hr = SHParseDisplayName(linkPath, nullptr, &shortcutPidl, 0, nullptr);
    ok &= Check(SUCCEEDED(hr), "Shortcut PIDL parsed");
    if (SUCCEEDED(hr))
    {
        CComPtr<IShellItem> source, target;
        hr = SHCreateItemFromIDList(shortcutPidl, IID_IShellItem, (void**)&source);
        ok &= Check(SUCCEEDED(hr), "Shortcut shell item obtained");
        if (SUCCEEDED(hr))
        {
            hr = source->BindToHandler(nullptr, BHID_LinkTargetItem, IID_IShellItem, (void**)&target);
            ok &= Check(SUCCEEDED(hr), "BHID_LinkTargetItem resolves shortcut");
            if (SUCCEEDED(hr))
            {
                hr = SHGetIDListFromObject(target, &targetPidl);
                ok &= Check(SUCCEEDED(hr), "Target PIDL obtained");
                if (SUCCEEDED(hr))
                {
                    wchar_t resultingPath[MAX_PATH]{};
                    ok &= Check(SHGetPathFromIDListW(targetPidl, resultingPath) != FALSE, "Target PIDL converts to path");
                    wprintf(L"EXPECTED_TARGET=%ls\nRESOLVED_TARGET=%ls\n", targetPath, resultingPath);
                    PIDLIST_ABSOLUTE expectedPidl = nullptr;
                    HRESULT expectedHr = SHParseDisplayName(targetPath, nullptr, &expectedPidl, 0, nullptr);
                    ok &= Check(SUCCEEDED(expectedHr), "Expected target PIDL parsed");
                    ok &= Check(expectedPidl && ILIsEqual(expectedPidl, targetPidl), "Open file location selects actual target");
                    CoTaskMemFree(expectedPidl);
                }
            }
        }
    }

    hr = SHParseDisplayName(targetPath, nullptr, &regularPidl, 0, nullptr);
    ok &= Check(SUCCEEDED(hr), "Regular file PIDL parsed");
    if (SUCCEEDED(hr))
    {
        CComPtr<IShellItem> regular, unexpectedTarget;
        hr = SHCreateItemFromIDList(regularPidl, IID_IShellItem, (void**)&regular);
        ok &= Check(SUCCEEDED(hr), "Regular file shell item obtained");
        if (SUCCEEDED(hr))
        {
            hr = regular->BindToHandler(nullptr, BHID_LinkTargetItem, IID_IShellItem, (void**)&unexpectedTarget);
            ok &= Check(FAILED(hr), "Regular file uses original PIDL fallback");
        }
    }
    CoTaskMemFree(shortcutPidl);
    CoTaskMemFree(targetPidl);
    CoTaskMemFree(regularPidl);
    DeleteFileW(linkPath);
    DeleteFileW(targetPath);
    RemoveDirectoryW(dir);
    CoUninitialize();
    printf("ISSUE2564_LINK_TARGET_RESULT=%s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 3;
}
