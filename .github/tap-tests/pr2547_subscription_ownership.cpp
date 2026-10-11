#include <windows.h>
#include <ocidl.h>
#include <xamlom.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <thread>
#include <vector>

static const CLSID TapClsid =
    {0x7d15741f, 0x2f3b, 0x4971, {0xb8,0x91,0x6a,0x5d,0x42,0xd7,0x1a,0x34}};

static void Check(bool ok, const char* label) {
    if (!ok) { std::cerr << "FAIL: " << label << std::endl; ExitProcess(2); }
}

class MockVisual final : public IVisualTreeService {
public:
    std::atomic<ULONG> refs{1};
    std::atomic<int> adviseCalls{0}, unadviseCalls{0}, live{0}, maxLive{0};
    std::atomic<bool> failAdvise{false}, failUnadvise{false};
    bool blockFirst = true;
    bool entered = false, proceed = false;
    bool blockUnadvise = false, unadviseEntered = false, proceedUnadvise = false;
    std::mutex mutex;
    std::condition_variable cv;
    IVisualTreeServiceCallback* callback = nullptr;

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** ppv) override {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        if (iid != IID_IUnknown && iid != __uuidof(IVisualTreeService)) return E_NOINTERFACE;
        *ppv = static_cast<IVisualTreeService*>(this);
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG n = --refs;
        if (!n) delete this;
        return n;
    }
    HRESULT STDMETHODCALLTYPE AdviseVisualTreeChange(IVisualTreeServiceCallback* cb) override {
        int count = ++adviseCalls;
        {
            std::unique_lock<std::mutex> lock(mutex);
            if (blockFirst && count == 1) {
                entered = true;
                cv.notify_all();
                cv.wait(lock, [&]{ return proceed; });
            }
            if (failAdvise) return E_FAIL;
            Check(callback == nullptr, "second active callback installed");
            callback = cb;
            callback->AddRef();
        }
        int now = ++live;
        int observed = maxLive.load();
        while (observed < now && !maxLive.compare_exchange_weak(observed, now)) {}
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE UnadviseVisualTreeChange(IVisualTreeServiceCallback* cb) override {
        ++unadviseCalls;
        std::unique_lock<std::mutex> lock(mutex);
        if (blockUnadvise) {
            unadviseEntered = true;
            cv.notify_all();
            cv.wait(lock, [&]{return proceedUnadvise;});
        }
        if (failUnadvise) return E_FAIL;
        if (callback != cb) return E_INVALIDARG;
        callback->Release();
        callback = nullptr;
        --live;
        return S_OK;
    }
    void WaitEntered() {
        std::unique_lock<std::mutex> lock(mutex);
        Check(cv.wait_for(lock, std::chrono::seconds(15), [&]{return entered;}),
              "first Advise did not begin");
    }
    void Resume() {
        std::lock_guard<std::mutex> lock(mutex);
        proceed = true;
        cv.notify_all();
    }
    void WaitUnadvise() {
        std::unique_lock<std::mutex> lock(mutex);
        Check(cv.wait_for(lock, std::chrono::seconds(15), [&]{return unadviseEntered;}),
              "shutdown did not enter Unadvise");
    }
    void ResumeUnadvise() {
        std::lock_guard<std::mutex> lock(mutex);
        proceedUnadvise = true;
        cv.notify_all();
    }
    HRESULT STDMETHODCALLTYPE GetEnums(unsigned int*, EnumType**) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE CreateInstance(BSTR, BSTR, InstanceHandle*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetPropertyValuesChain(InstanceHandle, unsigned int*,
                  PropertyChainSource**, unsigned int*, PropertyChainValue**) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetProperty(InstanceHandle, InstanceHandle, unsigned int) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE ClearProperty(InstanceHandle, unsigned int) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetCollectionCount(InstanceHandle, unsigned int*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetCollectionElements(InstanceHandle, unsigned int,
                  unsigned int*, CollectionElementValue**) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE AddChild(InstanceHandle, InstanceHandle, unsigned int) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE RemoveChild(InstanceHandle, unsigned int) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE ClearChildren(InstanceHandle) override { return E_NOTIMPL; }
};

using DllGetClassObjectFn = HRESULT (WINAPI *)(REFCLSID, REFIID, LPVOID*);
using TapStartFn = void (WINAPI *)(BOOL, BOOL);
using TapStopFn = void (WINAPI *)();

int main() {
    HRESULT init = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    Check(SUCCEEDED(init), "COM initialized");
    HMODULE lib = LoadLibraryExW(L"StartMenuHelper64.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    Check(lib != nullptr, "StartMenuHelper64 loads");
    auto getFactory = reinterpret_cast<DllGetClassObjectFn>(GetProcAddress(lib, "DllGetClassObject"));
    auto start = reinterpret_cast<TapStartFn>(GetProcAddress(lib, "StartWin11StartButtonTap"));
    auto stop = reinterpret_cast<TapStopFn>(GetProcAddress(lib, "StopWin11StartButtonTap"));
    Check(getFactory && start && stop, "real TAP DLL exports loaded");
    IClassFactory* factory = nullptr;
    Check(SUCCEEDED(getFactory(TapClsid, IID_IClassFactory, (void**)&factory)), "TAP factory obtained");
    auto newTap = [&] {
        IObjectWithSite* tap = nullptr;
        Check(SUCCEEDED(factory->CreateInstance(nullptr, IID_IObjectWithSite,
                         reinterpret_cast<void**>(&tap))), "TAP instance created");
        return tap;
    };
    auto* service = new MockVisual;
    IObjectWithSite* a = newTap();
    IObjectWithSite* b = newTap();
    start(FALSE, FALSE); // Enable the session without launching diagnostics worker.
    HRESULT firstResult = E_FAIL;
    std::thread first([&] { firstResult = a->SetSite(service); });
    service->WaitEntered();
    HRESULT secondResult = b->SetSite(service);
    Check(secondResult == HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS),
          "competing TAP rejected before Advise");
    Check(service->adviseCalls == 1, "exactly one Advise attempt during contention");
    service->Resume();
    first.join();
    Check(firstResult == S_OK, "first TAP registration succeeded");
    Check(service->live == 1 && service->maxLive == 1, "only one live subscription");
    stop();
    Check(service->live == 0 && service->unadviseCalls == 1,
          "successful shutdown unregisters owner");
    a->Release();
    b->Release();

    // If registration fails, its reservation must be released for a successor.
    IObjectWithSite* failed = newTap();
    IObjectWithSite* successor = newTap();
    service->blockFirst = false;
    service->failAdvise = true;
    start(FALSE, FALSE);
    Check(FAILED(failed->SetSite(service)), "injected Advise failure observed");
    service->failAdvise = false;
    Check(SUCCEEDED(successor->SetSite(service)), "successor accepts released reservation");
    Check(service->live == 1 && service->maxLive == 1,
          "failure rollback never allows overlapping subscription");
    stop();
    Check(service->live == 0, "successor unsubscribed");
    failed->Release();
    successor->Release();

    // An unsuccessful Unadvise must preserve the original owner until retry.
    IObjectWithSite* retained = newTap();
    IObjectWithSite* contender = newTap();
    start(FALSE, FALSE);
    Check(SUCCEEDED(retained->SetSite(service)), "retained TAP registered");
    int callsBefore = service->adviseCalls;
    service->failUnadvise = true;
    stop();
    Check(service->live == 1, "callback remains registered after failed Unadvise");
    start(FALSE, FALSE);
    Check(contender->SetSite(service) == HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS),
          "competitor rejected while failed Unadvise retains owner");
    Check(service->adviseCalls == callsBefore, "failed Unadvise cannot create second callback");
    service->failUnadvise = false;
    stop();
    Check(service->live == 0, "later Unadvise releases retained callback");
    retained->Release();
    contender->Release();

    // A burst of contenders must not replace an already subscribed owner.
    // Keep the owner on this thread so its message-only dispatch survives
    // until shutdown; the contenders can race on independent worker threads.
    constexpr int contenders = 24;
    start(FALSE, FALSE);
    IObjectWithSite* burstOwner = newTap();
    Check(SUCCEEDED(burstOwner->SetSite(service)), "burst owner registered");
    const int beforeBurst = service->adviseCalls;
    std::vector<IObjectWithSite*> competing;
    competing.reserve(contenders);
    for (int i = 0; i < contenders; ++i) competing.push_back(newTap());
    std::vector<HRESULT> outcomes(contenders, E_UNEXPECTED);
    std::vector<std::thread> workers;
    std::atomic<int> ready{0};
    std::atomic<bool> go{false};
    for (int i = 0; i < contenders; ++i) {
        workers.emplace_back([&, i] {
            ++ready;
            while (!go) std::this_thread::yield();
            outcomes[i] = competing[i]->SetSite(service);
        });
    }
    while (ready != contenders) std::this_thread::yield();
    go = true;
    for (auto& worker : workers) worker.join();
    for (HRESULT result : outcomes)
        Check(result == HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS),
              "concurrent contender rejected");
    Check(service->adviseCalls == beforeBurst && service->live == 1,
          "contenders did not register callbacks");
    stop();
    Check(service->live == 0, "burst owner unsubscribed");
    for (auto* tap : competing) tap->Release();
    burstOwner->Release();

    // A valid successor must be able to claim ownership after a clean stop.
    start(FALSE, FALSE);
    IObjectWithSite* afterBurst = newTap();
    Check(SUCCEEDED(afterBurst->SetSite(service)),
          "successor registered after burst cleanup");
    Check(service->live == 1, "successor owns one callback");
    stop();
    Check(service->live == 0, "successor removed its callback");
    afterBurst->Release();

    // Regression: Start while Stop is inside Unadvise. The old owner must
    // be unpublished after successful shutdown, even if Start toggled active.
    IObjectWithSite* raceOwner = newTap();
    start(FALSE, FALSE);
    std::mutex gateMutex;
    std::condition_variable gateCv;
    bool ownerReady = false, requestStop = false;
    HRESULT raceSetup = E_FAIL;
    {
        std::lock_guard<std::mutex> lock(service->mutex);
        service->blockUnadvise = true;
        service->unadviseEntered = false;
        service->proceedUnadvise = false;
    }
    std::thread stopper([&] {
        raceSetup = raceOwner->SetSite(service);
        {
            std::unique_lock<std::mutex> lock(gateMutex);
            ownerReady = true;
            gateCv.notify_all();
            gateCv.wait(lock, [&]{return requestStop;});
        }
        stop();  // Same thread that created the dispatch window.
    });
    {
        std::unique_lock<std::mutex> lock(gateMutex);
        Check(gateCv.wait_for(lock, std::chrono::seconds(15), [&]{return ownerReady;}),
              "race TAP setup did not complete");
        Check(SUCCEEDED(raceSetup), "race TAP owner subscribed");
        requestStop = true;
    }
    gateCv.notify_all();
    service->WaitUnadvise();
    std::atomic<bool> startEntered{false};
    std::thread starter([&] {
        startEntered = true;
        start(FALSE, FALSE);
    });
    while (!startEntered) std::this_thread::yield();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    service->ResumeUnadvise();
    stopper.join();
    starter.join();
    Check(service->live == 0, "race shutdown unsubscribed");
    IObjectWithSite* raceSuccessor = newTap();
    Check(SUCCEEDED(raceSuccessor->SetSite(service)),
          "late Start did not strand the old unsubscribed TAP owner");
    stop();
    Check(service->live == 0, "race successor unsubscribed");
    raceSuccessor->Release();
    raceOwner->Release();
    service->blockUnadvise = false;

    // SetSite(nullptr) must use the same teardown as Stop: no stale
    // subscription or published TAP owner may survive detachment.
    IObjectWithSite* siteOwner = newTap();
    start(FALSE, FALSE);
    Check(SUCCEEDED(siteOwner->SetSite(service)), "SetSite attached diagnostics site");
    IUnknown* queriedSite = nullptr;
    Check(SUCCEEDED(siteOwner->GetSite(IID_IUnknown, (void**)&queriedSite)) && queriedSite,
          "GetSite exposes attached COM site");
    queriedSite->Release();
    Check(siteOwner->SetSite(service) == HRESULT_FROM_WIN32(ERROR_ALREADY_INITIALIZED),
          "second non-null SetSite rejected");
    Check(SUCCEEDED(siteOwner->SetSite(nullptr)), "SetSite(nullptr) detached site");
    Check(service->live == 0, "SetSite(nullptr) removed the subscription");
    queriedSite = reinterpret_cast<IUnknown*>(1);
    Check(FAILED(siteOwner->GetSite(IID_IUnknown, (void**)&queriedSite)) && !queriedSite,
          "GetSite returns no site after detachment");
    IObjectWithSite* afterDetach = newTap();
    Check(SUCCEEDED(afterDetach->SetSite(service)),
          "new TAP can attach after SetSite(nullptr)");
    Check(service->live == 1, "one subscription after site replacement");
    stop();
    Check(service->live == 0, "replacement TAP unsubscribed on Stop");
    siteOwner->Release();
    afterDetach->Release();

    Check(service->refs == 1, "all service COM references balanced");
    service->Release();
    factory->Release();
    FreeLibrary(lib);
    CoUninitialize();
    std::cout << "PASS: real TAP DLL prevents dual subscriptions; fault rollback and COM references verified" << std::endl;
    return 0;
}
