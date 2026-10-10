// Isolated Windows 11 x64/ARM64 COM stress test. Not part of Open-Shell production.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <ocidl.h>
#include <iostream>

static const GUID CLSID_OpenShellStartButtonTap =
{ 0x7d15741f, 0x2f3b, 0x4971, { 0xb8, 0x91, 0x6a, 0x5d, 0x42, 0xd7, 0x1a, 0x34 } };

class DummySite final : public IUnknown
{
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override
    {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (riid != IID_IUnknown) return E_NOINTERFACE;
        *out = static_cast<IUnknown*>(this);
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
    ULONG STDMETHODCALLTYPE Release() override { return --refs; }
private:
    ULONG refs = 1;
};

static HWND FindTapWindow()
{
    return FindWindowExW(HWND_MESSAGE, nullptr, L"OpenShell.Win11StartButtonTap", nullptr);
}

static int fail(const wchar_t* what, HRESULT hr)
{
    std::wcerr << L"FAIL " << what << L": 0x" << std::hex << hr << std::endl;
    return 1;
}

int wmain(int argc, wchar_t* argv[])
{
    if (argc != 2) return fail(L"expected absolute DLL path", E_INVALIDARG);
    HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(init)) return fail(L"CoInitializeEx", init);
    HMODULE library = LoadLibraryExW(argv[1], nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!library) return fail(L"LoadLibraryEx", HRESULT_FROM_WIN32(GetLastError()));

    using GetFactoryFn = HRESULT (STDAPICALLTYPE*)(REFCLSID, REFIID, void**);
    using CanUnloadFn = HRESULT (STDAPICALLTYPE*)();
    auto getFactory = reinterpret_cast<GetFactoryFn>(
        GetProcAddress(library, "DllGetClassObject"));
    auto canUnload = reinterpret_cast<CanUnloadFn>(
        GetProcAddress(library, "DllCanUnloadNow"));
    if (!getFactory || !canUnload) return fail(L"missing standard COM exports", E_FAIL);

    IClassFactory* factory = nullptr;
    HRESULT hr = getFactory(CLSID_OpenShellStartButtonTap, IID_IClassFactory,
        reinterpret_cast<void**>(&factory));
    if (FAILED(hr) || !factory) return fail(L"DllGetClassObject", hr);

    for (int i = 0; i < 400; ++i)
    {
        IObjectWithSite* site = nullptr;
        hr = factory->CreateInstance(nullptr, IID_IObjectWithSite, reinterpret_cast<void**>(&site));
        if (FAILED(hr) || !site) return fail(L"CreateInstance(IObjectWithSite)", hr);
        // The upstream TAP creates the message-only window only after a
        // valid XAML site has been assigned and Advise succeeds.
        if (FindTapWindow()) return fail(L"constructor created HWND before SetSite", E_FAIL);

        IUnknown* currentSite = nullptr;
        hr = site->GetSite(IID_IUnknown, reinterpret_cast<void**>(&currentSite));
        if (hr != E_FAIL || currentSite) return fail(L"unexpected site before SetSite", hr);

        hr = site->SetSite(nullptr);
        if (FAILED(hr)) return fail(L"SetSite(nullptr)", hr);
        hr = site->SetSite(nullptr);
        if (FAILED(hr)) return fail(L"SetSite(nullptr) twice: non-idempotent shutdown", hr);
        if (FindTapWindow()) return fail(L"dispatch HWND leaked after SetSite(nullptr)", E_FAIL);
        IUnknown* clearedSite = nullptr;
        hr = site->GetSite(IID_IUnknown, reinterpret_cast<void**>(&clearedSite));
        if (hr != E_FAIL || clearedSite) return fail(L"site retained after shutdown", hr);
        site->Release();
    }

    // Exercise the non-null SetSite error path: QueryInterface for the
    // visual tree interface must fail, and its HWND must not leak.
    IObjectWithSite* invalid = nullptr;
    hr = factory->CreateInstance(nullptr, IID_IObjectWithSite,
        reinterpret_cast<void**>(&invalid));
    if (FAILED(hr) || !invalid) return fail(L"CreateInstance(invalid site)", hr);
    DummySite dummy;
    hr = invalid->SetSite(&dummy);
    if (hr != E_NOINTERFACE) return fail(L"invalid visual tree site must be rejected", hr);
    if (FindTapWindow()) return fail(L"dispatch HWND leaked on invalid site", E_FAIL);
    invalid->Release();

    // An unsupported interface creates then disposes a TAP.
    IStream* unsupported = nullptr;
    hr = factory->CreateInstance(nullptr, IID_IStream,
        reinterpret_cast<void**>(&unsupported));
    if (hr != E_NOINTERFACE || unsupported)
        return fail(L"unsupported COM interface did not fail safely", hr);
    if (FindTapWindow()) return fail(L"dispatch HWND leaked on unsupported IID", E_FAIL);

    hr = factory->LockServer(TRUE);
    if (FAILED(hr)) return fail(L"LockServer(TRUE)", hr);
    if (canUnload() != S_FALSE) return fail(L"DllCanUnloadNow did not honor LockServer(TRUE)", E_FAIL);
    hr = factory->LockServer(FALSE);
    if (FAILED(hr)) return fail(L"LockServer(FALSE)", hr);
    factory->Release();
    hr = canUnload();
    if (hr != S_OK) return fail(L"DllCanUnloadNow should report no active TAP", hr);
    FreeLibrary(library);
    CoUninitialize();
    std::wcout << L"PASS: 400 COM/dispatch lifetime cycles, repeated SetSite(nullptr), "
                  L"invalid site, unsupported interface, LockServer and unload eligibility" << std::endl;
    return 0;
}
