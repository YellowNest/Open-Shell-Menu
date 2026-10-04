// Windows 11 native Start button suppression.
//
// Open-Shell's replacement button is a separate layered window. Windows 11
// renders its own Start button in XAML, so the old HWND hiding logic cannot
// remove the native glyph. This file uses the public XAML diagnostics API to
// suppress only the native Start glyph and its hit target. The XAML element is
// left in layout, so centered taskbar positioning remains owned by Windows.

#include "stdafx.h"
#include "Win11StartButton.h"
#include "Settings.h"
#include "LogManager.h"
#include "ResourceHelper.h"

#include <Windows.UI.Xaml.h>
#include <Windows.UI.Xaml.Input.h>
#include <Windows.UI.Input.h>
#include <Windows.Devices.Input.h>
#include <xamlom.h>
#include <ocidl.h>
#include <roapi.h>

#pragma comment(lib, "runtimeobject.lib")
#include <unordered_map>
#include <vector>

static const GUID CLSID_OpenShellStartButtonTap =
{ 0x7d15741f, 0x2f3b, 0x4971, { 0xb8, 0x91, 0x6a, 0x5d, 0x42, 0xd7, 0x1a, 0x34 } };

static const UINT WM_OS_STARTBUTTON_APPLY = WM_APP + 0x35B;
static const UINT WM_OS_STARTBUTTON_DESTROY = WM_APP + 0x35C;

static volatile LONG g_StartButtonActive = 0;
static volatile LONG g_ConnectStarted = 0;

static HMODULE GetThisModule( void )
{
	HMODULE module = NULL;
	GetModuleHandleEx(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
		(LPCTSTR)&GetThisModule, &module);
	return module;
}

struct StartElement
{
	InstanceHandle parent;
	CString type;
	CString name;
	bool visibilityOverride;
	bool hitTestOverride;
	bool startControlResolved;
	bool isStartControl;
	unsigned int discoveryOrder;

	StartElement( void )
	{
		parent = 0;
		visibilityOverride = false;
		hitTestOverride = false;
		startControlResolved = false;
		isStartControl = false;
		discoveryOrder = 0;
	}
};

class CWin11StartButtonTap;

enum StartPointerProbeEvent
{
	START_POINTER_PRESSED,
	START_POINTER_RELEASED,
	START_POINTER_CANCELED,
	START_POINTER_CAPTURE_LOST,
};

class CStartPointerProbeHandler:
	public IInspectable,
	public ABI::Windows::UI::Xaml::Input::IPointerEventHandler
{
public:
	CStartPointerProbeHandler( CWin11StartButtonTap *owner, StartPointerProbeEvent eventType )
	{
		m_Refs = 1;
		m_Owner = owner;
		m_EventType = eventType;
	}

	STDMETHODIMP QueryInterface( REFIID riid, void **ppv )
	{
		if (!ppv)
			return E_POINTER;
		*ppv = NULL;

		if (riid == IID_IUnknown || riid == IID_IInspectable)
			*ppv = static_cast<IInspectable*>(this);
		else if (riid == __uuidof(ABI::Windows::UI::Xaml::Input::IPointerEventHandler))
			*ppv = static_cast<ABI::Windows::UI::Xaml::Input::IPointerEventHandler*>(this);
		else
			return E_NOINTERFACE;

		AddRef();
		return S_OK;
	}

	STDMETHODIMP_(ULONG) AddRef( void )
	{
		return (ULONG)InterlockedIncrement(&m_Refs);
	}

	STDMETHODIMP_(ULONG) Release( void )
	{
		LONG refs = InterlockedDecrement(&m_Refs);
		if (!refs)
			delete this;
		return (ULONG)refs;
	}

	STDMETHODIMP GetIids( ULONG *iidCount, IID **iids )
	{
		if (!iidCount || !iids)
			return E_POINTER;

		*iidCount = 1;
		*iids = (IID*)CoTaskMemAlloc(sizeof(IID));
		if (!*iids)
		{
			*iidCount = 0;
			return E_OUTOFMEMORY;
		}
		(*iids)[0] = __uuidof(ABI::Windows::UI::Xaml::Input::IPointerEventHandler);
		return S_OK;
	}

	STDMETHODIMP GetRuntimeClassName( HSTRING *className )
	{
		if (!className)
			return E_POINTER;
		*className = NULL;
		return S_OK;
	}

	STDMETHODIMP GetTrustLevel( TrustLevel *trustLevel )
	{
		if (!trustLevel)
			return E_POINTER;
		*trustLevel = BaseTrust;
		return S_OK;
	}

	STDMETHODIMP Invoke( IInspectable *sender,
		ABI::Windows::UI::Xaml::Input::IPointerRoutedEventArgs *args );

private:
	LONG m_Refs;
	CWin11StartButtonTap *m_Owner;
	StartPointerProbeEvent m_EventType;
};

static CWin11StartButtonTap *g_Tap = NULL;
static SRWLOCK g_TapLock = SRWLOCK_INIT;

static bool ContainsText( const CString &text, const wchar_t *part )
{
	return text.Find(part) >= 0;
}

static bool IsStartControlCandidate( const StartElement &element )
{
	if (!ContainsText(element.type, L"ExperienceToggleButton"))
		return false;
	return element.name == L"LaunchListButton" || element.name == L"StartButton";
}

static bool IsStartGlyph( const StartElement &element )
{
	if (element.name == L"Icon")
		return true;
	if (ContainsText(element.type, L"AnimatedVisualPlayer") || ContainsText(element.type, L"AepAnimatedIcon"))
		return true;
	if (ContainsText(element.type, L"FontIcon") || ContainsText(element.type, L"PathIcon") ||
		ContainsText(element.type, L"ImageIcon") || ContainsText(element.type, L"BitmapIcon") ||
		ContainsText(element.type, L"SymbolIcon"))
		return true;
	return false;
}

class CWin11StartButtonTap: public IObjectWithSite, public IVisualTreeServiceCallback2
{
public:
	CWin11StartButtonTap( void )
	{
		m_Refs = 1;
		m_Advised = false;
		m_Dispatch = NULL;
		m_PrimaryStart = 0;
		m_ProbeStart = 0;
		m_NextDiscoveryOrder = 0;
		m_InjectionReferenceBalanced = false;
		InitializeCriticalSection(&m_Lock);
	}

	~CWin11StartButtonTap( void )
	{
		DetachPointerProbe();
		if (m_Visual && m_Advised)
			m_Visual->UnadviseVisualTreeChange(static_cast<IVisualTreeServiceCallback*>(this));
		if (m_Dispatch && GetWindowThreadProcessId(m_Dispatch, NULL) == GetCurrentThreadId())
			DestroyWindow(m_Dispatch);

		AcquireSRWLockExclusive(&g_TapLock);
		if (g_Tap == this)
			g_Tap = NULL;
		ReleaseSRWLockExclusive(&g_TapLock);

		InterlockedExchange(&g_ConnectStarted, 0);
		DeleteCriticalSection(&m_Lock);
	}

	STDMETHODIMP QueryInterface( REFIID riid, void **ppv )
	{
		if (!ppv)
			return E_POINTER;
		*ppv = NULL;

		if (riid == IID_IUnknown || riid == IID_IObjectWithSite)
			*ppv = static_cast<IObjectWithSite*>(this);
		else if (riid == __uuidof(IVisualTreeServiceCallback) || riid == __uuidof(IVisualTreeServiceCallback2))
			*ppv = static_cast<IVisualTreeServiceCallback2*>(this);
		else
			return E_NOINTERFACE;

		AddRef();
		return S_OK;
	}

	STDMETHODIMP_(ULONG) AddRef( void )
	{
		return (ULONG)InterlockedIncrement(&m_Refs);
	}

	STDMETHODIMP_(ULONG) Release( void )
	{
		LONG refs = InterlockedDecrement(&m_Refs);
		if (!refs)
			delete this;
		return (ULONG)refs;
	}

	STDMETHODIMP SetSite( IUnknown *site )
	{
		if (m_Visual && m_Advised)
		{
			ApplyState(false);
			m_Visual->UnadviseVisualTreeChange(static_cast<IVisualTreeServiceCallback*>(this));
			m_Advised = false;
		}
		DetachPointerProbe();
		m_Visual.Release();
		m_Diagnostics.Release();
		m_UIElementStatics.Release();
		m_Site.Release();

		if (!site)
		{
			AcquireSRWLockExclusive(&g_TapLock);
			if (g_Tap == this)
				g_Tap = NULL;
			ReleaseSRWLockExclusive(&g_TapLock);
			InterlockedExchange(&g_ConnectStarted, 0);
			return S_OK;
		}

		EnterCriticalSection(&m_Lock);
		m_Elements.clear();
		m_PrimaryStart = 0;
		m_NextDiscoveryOrder = 0;
		LeaveCriticalSection(&m_Lock);

		m_Site = site;

		// InitializeXamlDiagnosticsEx loads the TAP DLL by path even though
		// StartMenuDLL is already loaded. Balance that injection reference so
		// Open-Shell can unload and initialize the DLL again after an explicit Exit.
		if (!m_InjectionReferenceBalanced)
		{
			HMODULE module = GetThisModule();
			if (module)
			{
				FreeLibrary(module);
				m_InjectionReferenceBalanced = true;
			}
		}

		// Exit may race a connection attempt. Do not attach a new callback after
		// shutdown has already begun.
		if (!InterlockedCompareExchange(&g_StartButtonActive, 0, 0))
		{
			m_Site.Release();
			InterlockedExchange(&g_ConnectStarted, 0);
			return S_OK;
		}

		HRESULT hr = site->QueryInterface(__uuidof(IVisualTreeService), (void**)&m_Visual);
		if (FAILED(hr) || !m_Visual)
			return hr;

		// IXamlDiagnostics lets the TAP turn visual-tree handles back into the
		// actual XAML objects. Keep this optional so the existing visual
		// suppression still works if a future build stops exposing it.
		HRESULT diagnosticsHr = site->QueryInterface(__uuidof(IXamlDiagnostics), (void**)&m_Diagnostics);
		LogToFile(STARTUP_LOG, L"Win11StartButton: IXamlDiagnostics 0x%08X", diagnosticsHr);

		if (!CreateDispatchWindow())
			return HRESULT_FROM_WIN32(GetLastError());

		AcquireSRWLockExclusive(&g_TapLock);
		g_Tap = this;
		ReleaseSRWLockExclusive(&g_TapLock);

		// Advise replays the existing tree. OnVisualTreeChange only records
		// element handles; all property access is dispatched afterwards.
		hr = m_Visual->AdviseVisualTreeChange(static_cast<IVisualTreeServiceCallback*>(this));
		if (SUCCEEDED(hr))
		{
			m_Advised = true;
			RequestApply(false);
		}
		LogToFile(STARTUP_LOG, L"Win11StartButton: visual tree advise 0x%08X", hr);
		return hr;
	}

	STDMETHODIMP GetSite( REFIID riid, void **ppv )
	{
		if (!ppv)
			return E_POINTER;
		*ppv = NULL;
		if (!m_Site)
			return E_FAIL;
		return m_Site->QueryInterface(riid, ppv);
	}

	STDMETHODIMP OnVisualTreeChange( ParentChildRelation relation, VisualElement element, VisualMutationType mutationType )
	{
		bool interesting = false;

		EnterCriticalSection(&m_Lock);
		if (mutationType == Remove)
		{
			auto it = m_Elements.find(element.Handle);
			if (it != m_Elements.end())
			{
				interesting = it->second.visibilityOverride || it->second.hitTestOverride ||
					it->second.isStartControl || IsStartControlCandidate(it->second);
				bool wasPrimary = element.Handle == m_PrimaryStart;
				m_Elements.erase(it);
				if (wasPrimary)
					m_PrimaryStart = FindPrimaryStartLocked();
			}
		}
		else if (mutationType == Add)
		{
			StartElement record;
			record.parent = relation.Parent;
			record.type = element.Type ? element.Type : L"";
			record.name = element.Name ? element.Name : L"";

			auto previous = m_Elements.find(element.Handle);
			if (previous != m_Elements.end())
			{
				record.visibilityOverride = previous->second.visibilityOverride;
				record.hitTestOverride = previous->second.hitTestOverride;
				record.startControlResolved = previous->second.startControlResolved;
				record.isStartControl = previous->second.isStartControl;
				record.discoveryOrder = previous->second.discoveryOrder;
			}
			else
			{
				record.discoveryOrder = ++m_NextDiscoveryOrder;
			}
			m_Elements[element.Handle] = record;

			interesting = IsStartControlCandidate(record) || IsStartGlyph(record) ||
				IsUnderStartButtonLocked(record.parent);
		}
		LeaveCriticalSection(&m_Lock);

		if (interesting)
			RequestApply(false);
		return S_OK;
	}

	STDMETHODIMP OnElementStateChanged( InstanceHandle, VisualElementState, LPCWSTR )
	{
		return S_OK;
	}

	void RequestApply( bool synchronous )
	{
		if (!m_Dispatch)
			return;
		if (synchronous)
			SendMessage(m_Dispatch, WM_OS_STARTBUTTON_APPLY, 0, 0);
		else
			PostMessage(m_Dispatch, WM_OS_STARTBUTTON_APPLY, 0, 0);
	}

	void Shutdown( void )
	{
		// Prevent new users of the TAP while teardown is in progress. The caller
		// holds a reference, so UnadviseVisualTreeChange cannot destroy this object
		// out from under the shutdown sequence.
		AcquireSRWLockExclusive(&g_TapLock);
		if (g_Tap == this)
			g_Tap = NULL;
		ReleaseSRWLockExclusive(&g_TapLock);

		// Pointer handlers are attached to live XAML objects and must be removed
		// before the visual-tree callback/site is detached.
		DetachPointerProbe();

		// Restore the native XAML state before detaching the callback.
		RequestApply(true);

		if (m_Visual && m_Advised)
		{
			HRESULT hr = m_Visual->UnadviseVisualTreeChange(static_cast<IVisualTreeServiceCallback*>(this));
			if (FAILED(hr))
			{
				// Keep the DLL resident rather than leave XAML with a callback into
				// unloaded code. Explorer restart remains the safe recovery path.
				HMODULE module = NULL;
				GetModuleHandleEx(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
					(LPCTSTR)&GetThisModule, &module);
				LogToFile(STARTUP_LOG, L"Win11StartButton: visual tree unadvise failed 0x%08X", hr);
				return;
			}
			m_Advised = false;
		}

		m_Visual.Release();
		m_Diagnostics.Release();
		m_UIElementStatics.Release();
		m_Site.Release();
		InterlockedExchange(&g_ConnectStarted, 0);

		if (m_Dispatch)
			SendMessage(m_Dispatch, WM_OS_STARTBUTTON_DESTROY, 0, 0);
	}

private:
	friend class CStartPointerProbeHandler;

	static LRESULT CALLBACK DispatchProc( HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam )
	{
		CWin11StartButtonTap *tap = (CWin11StartButtonTap*)GetWindowLongPtr(hwnd, GWLP_USERDATA);
		if (msg == WM_NCCREATE)
		{
			CREATESTRUCT *create = (CREATESTRUCT*)lParam;
			tap = (CWin11StartButtonTap*)create->lpCreateParams;
			SetWindowLongPtr(hwnd, GWLP_USERDATA, (LONG_PTR)tap);
		}
		if (msg == WM_OS_STARTBUTTON_APPLY && tap)
		{
			bool enabled = InterlockedCompareExchange(&g_StartButtonActive, 0, 0) != 0 &&
				GetSettingBool(L"EnableStartButton");
			tap->ApplyState(enabled);
			return 0;
		}
		if (msg == WM_OS_STARTBUTTON_DESTROY && tap)
		{
			tap->m_Dispatch = NULL;
			SetWindowLongPtr(hwnd, GWLP_USERDATA, 0);
			DestroyWindow(hwnd);
			return 0;
		}
		return DefWindowProc(hwnd, msg, wParam, lParam);
	}

	bool CreateDispatchWindow( void )
	{
		if (m_Dispatch)
			return true;

		static const wchar_t CLASS_NAME[] = L"OpenShell.Win11StartButtonTap";
		WNDCLASS wc = {};
		HMODULE module = GetThisModule();
		if (!module)
			return false;

		wc.lpfnWndProc = DispatchProc;
		wc.hInstance = module;
		wc.lpszClassName = CLASS_NAME;
		if (!RegisterClass(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
			return false;

		m_Dispatch = CreateWindowEx(0, CLASS_NAME, L"", 0, 0, 0, 0, 0,
			HWND_MESSAGE, NULL, module, this);
		return m_Dispatch != NULL;
	}

	bool IsUnderStartButtonLocked( InstanceHandle parent ) const
	{
		for (int depth = 0; depth < 24 && parent; depth++)
		{
			auto it = m_Elements.find(parent);
			if (it == m_Elements.end())
				break;
			if (it->second.isStartControl)
				return true;
			parent = it->second.parent;
		}
		return false;
	}

	InstanceHandle GetStartAncestor( InstanceHandle handle )
	{
		InstanceHandle result = 0;
		EnterCriticalSection(&m_Lock);
		auto it = m_Elements.find(handle);
		if (it != m_Elements.end())
		{
			InstanceHandle parent = it->second.parent;
			for (int depth = 0; depth < 24 && parent; depth++)
			{
				auto pit = m_Elements.find(parent);
				if (pit == m_Elements.end())
					break;
				if (pit->second.isStartControl)
				{
					result = parent;
					break;
				}
				parent = pit->second.parent;
			}
		}
		LeaveCriticalSection(&m_Lock);
		return result;
	}

	static void FreeProperties( PropertyChainSource *sources, unsigned int sourceCount,
		PropertyChainValue *values, unsigned int valueCount )
	{
		if (sources)
		{
			for (unsigned int i = 0; i < sourceCount; i++)
			{
				SysFreeString(sources[i].TargetType);
				SysFreeString(sources[i].Name);
				SysFreeString(sources[i].SrcInfo.FileName);
				SysFreeString(sources[i].SrcInfo.Hash);
			}
			CoTaskMemFree(sources);
		}
		if (values)
		{
			for (unsigned int i = 0; i < valueCount; i++)
			{
				SysFreeString(values[i].Type);
				SysFreeString(values[i].DeclaringType);
				SysFreeString(values[i].ValueType);
				SysFreeString(values[i].ItemType);
				SysFreeString(values[i].Value);
				SysFreeString(values[i].PropertyName);
			}
			CoTaskMemFree(values);
		}
	}

	InstanceHandle FindPrimaryStartLocked( void ) const
	{
		InstanceHandle primary = 0;
		unsigned int bestOrder = 0;
		for (auto it = m_Elements.begin(); it != m_Elements.end(); ++it)
		{
			if (!it->second.isStartControl)
				continue;
			if (!primary || it->second.discoveryOrder < bestOrder)
			{
				primary = it->first;
				bestOrder = it->second.discoveryOrder;
			}
		}
		return primary;
	}

	HRESULT ResolveStartControl( InstanceHandle handle, const StartElement &element, bool *isStartControl )
	{
		*isStartControl = false;
		if (!IsStartControlCandidate(element))
			return S_OK;

		// Older taskbar implementations may expose a distinct x:Name.
		if (element.name.CompareNoCase(L"StartButton") == 0)
		{
			*isStartControl = true;
			return S_OK;
		}

		unsigned int sourceCount = 0;
		unsigned int valueCount = 0;
		PropertyChainSource *sources = NULL;
		PropertyChainValue *values = NULL;
		HRESULT hr = m_Visual->GetPropertyValuesChain(handle, &sourceCount, &sources, &valueCount, &values);
		if (FAILED(hr))
		{
			FreeProperties(sources, sourceCount, values, valueCount);
			return hr;
		}

		for (unsigned int i = 0; i < valueCount; i++)
		{
			if (!values[i].PropertyName || !values[i].Value)
				continue;
			if (_wcsicmp(values[i].PropertyName, L"AutomationId") != 0 &&
				_wcsicmp(values[i].PropertyName, L"AutomationProperties.AutomationId") != 0)
				continue;
			if (_wcsicmp(values[i].Value, L"StartButton") == 0)
			{
				*isStartControl = true;
				break;
			}
		}

		FreeProperties(sources, sourceCount, values, valueCount);
		return S_OK;
	}

	void SetControlClassification( InstanceHandle handle, bool isStartControl )
	{
		EnterCriticalSection(&m_Lock);
		auto it = m_Elements.find(handle);
		if (it != m_Elements.end())
		{
			it->second.startControlResolved = true;
			it->second.isStartControl = isStartControl;
			m_PrimaryStart = FindPrimaryStartLocked();
		}
		LeaveCriticalSection(&m_Lock);
	}

	HRESULT FindProperty( InstanceHandle handle, const wchar_t *name, unsigned int *index, CString *typeName )
	{
		unsigned int sourceCount = 0;
		unsigned int valueCount = 0;
		PropertyChainSource *sources = NULL;
		PropertyChainValue *values = NULL;
		HRESULT hr = m_Visual->GetPropertyValuesChain(handle, &sourceCount, &sources, &valueCount, &values);
		if (FAILED(hr))
		{
			FreeProperties(sources, sourceCount, values, valueCount);
			return hr;
		}

		bool found = false;
		for (unsigned int i = 0; i < valueCount; i++)
		{
			if (!values[i].PropertyName || wcscmp(values[i].PropertyName, name) != 0)
				continue;
			if (values[i].MetadataBits & 0x2) // IsPropertyReadOnly
				continue;

			*index = values[i].Index;
			if (typeName)
				*typeName = values[i].Type ? values[i].Type : L"";
			found = true;
			break;
		}
		FreeProperties(sources, sourceCount, values, valueCount);
		return found ? S_OK : HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
	}

	HRESULT SetPropertyText( InstanceHandle handle, const wchar_t *name, const wchar_t *valueText )
	{
		unsigned int index = 0;
		CString typeName;
		HRESULT hr = FindProperty(handle, name, &index, &typeName);
		if (FAILED(hr) || typeName.IsEmpty())
			return FAILED(hr) ? hr : E_FAIL;

		CComBSTR type(typeName);
		CComBSTR value(valueText);
		InstanceHandle created = 0;
		hr = m_Visual->CreateInstance(type, value, &created);
		if (FAILED(hr))
			return hr;
		return m_Visual->SetProperty(handle, created, index);
	}

	HRESULT ClearPropertyByName( InstanceHandle handle, const wchar_t *name )
	{
		unsigned int index = 0;
		HRESULT hr = FindProperty(handle, name, &index, NULL);
		if (FAILED(hr))
			return hr;
		return m_Visual->ClearProperty(handle, index);
	}

	void SetOverrideFlags( InstanceHandle handle, bool *visibility, bool *hitTest )
	{
		EnterCriticalSection(&m_Lock);
		auto it = m_Elements.find(handle);
		if (it != m_Elements.end())
		{
			if (visibility)
				it->second.visibilityOverride = *visibility;
			if (hitTest)
				it->second.hitTestOverride = *hitTest;
		}
		LeaveCriticalSection(&m_Lock);
	}

	HRESULT EnsureUIElementStatics( void )
	{
		if (m_UIElementStatics)
			return S_OK;

		HSTRING className = NULL;
		const wchar_t runtimeClass[] = L"Windows.UI.Xaml.UIElement";
		HRESULT hr = WindowsCreateString(runtimeClass, (UINT32)_countof(runtimeClass) - 1, &className);
		if (FAILED(hr))
			return hr;

		hr = RoGetActivationFactory(className, __uuidof(ABI::Windows::UI::Xaml::IUIElementStatics),
			(void**)&m_UIElementStatics);
		WindowsDeleteString(className);
		return hr;
	}

	HRESULT CreatePointerProbeHandler( StartPointerProbeEvent eventType, CComPtr<IInspectable> &handler )
	{
		CStartPointerProbeHandler *probe = new CStartPointerProbeHandler(this, eventType);
		if (!probe)
			return E_OUTOFMEMORY;
		HRESULT hr = probe->QueryInterface(IID_IInspectable, (void**)&handler);
		probe->Release();
		return hr;
	}

	void DetachPointerProbe( void )
	{
		if (m_ProbeElement)
		{
			if (m_ProbePressedEvent && m_ProbePressedHandler)
				m_ProbeElement->RemoveHandler(m_ProbePressedEvent, m_ProbePressedHandler);
			if (m_ProbeReleasedEvent && m_ProbeReleasedHandler)
				m_ProbeElement->RemoveHandler(m_ProbeReleasedEvent, m_ProbeReleasedHandler);
			if (m_ProbeCanceledEvent && m_ProbeCanceledHandler)
				m_ProbeElement->RemoveHandler(m_ProbeCanceledEvent, m_ProbeCanceledHandler);
			if (m_ProbeCaptureLostEvent && m_ProbeCaptureLostHandler)
				m_ProbeElement->RemoveHandler(m_ProbeCaptureLostEvent, m_ProbeCaptureLostHandler);
		}

		m_ProbePressedHandler.Release();
		m_ProbeReleasedHandler.Release();
		m_ProbeCanceledHandler.Release();
		m_ProbeCaptureLostHandler.Release();
		m_ProbePressedEvent.Release();
		m_ProbeReleasedEvent.Release();
		m_ProbeCanceledEvent.Release();
		m_ProbeCaptureLostEvent.Release();
		m_ProbeElement.Release();
		m_ProbeStart = 0;
	}

	HRESULT AttachPointerProbe( InstanceHandle startHandle )
	{
		if (!startHandle || !m_Diagnostics)
			return E_NOINTERFACE;
		if (m_ProbeStart == startHandle && m_ProbeElement)
			return S_OK;

		DetachPointerProbe();

		CComPtr<IInspectable> inspectable;
		HRESULT hr = m_Diagnostics->GetIInspectableFromHandle(startHandle, &inspectable);
		if (FAILED(hr) || !inspectable)
			return FAILED(hr) ? hr : E_FAIL;

		hr = inspectable->QueryInterface(__uuidof(ABI::Windows::UI::Xaml::IUIElement),
			(void**)&m_ProbeElement);
		if (FAILED(hr) || !m_ProbeElement)
			return FAILED(hr) ? hr : E_NOINTERFACE;

		hr = EnsureUIElementStatics();
		if (FAILED(hr))
		{
			DetachPointerProbe();
			return hr;
		}

		if (FAILED(hr = m_UIElementStatics->get_PointerPressedEvent(&m_ProbePressedEvent)) ||
			FAILED(hr = m_UIElementStatics->get_PointerReleasedEvent(&m_ProbeReleasedEvent)) ||
			FAILED(hr = m_UIElementStatics->get_PointerCanceledEvent(&m_ProbeCanceledEvent)) ||
			FAILED(hr = m_UIElementStatics->get_PointerCaptureLostEvent(&m_ProbeCaptureLostEvent)) ||
			FAILED(hr = CreatePointerProbeHandler(START_POINTER_PRESSED, m_ProbePressedHandler)) ||
			FAILED(hr = CreatePointerProbeHandler(START_POINTER_RELEASED, m_ProbeReleasedHandler)) ||
			FAILED(hr = CreatePointerProbeHandler(START_POINTER_CANCELED, m_ProbeCanceledHandler)) ||
			FAILED(hr = CreatePointerProbeHandler(START_POINTER_CAPTURE_LOST, m_ProbeCaptureLostHandler)))
		{
			DetachPointerProbe();
			return hr;
		}

		// handledEventsToo is intentional. ButtonBase class handling normally
		// consumes PointerPressed before ordinary instance handlers see it.
		if (FAILED(hr = m_ProbeElement->AddHandler(m_ProbePressedEvent, m_ProbePressedHandler, TRUE)) ||
			FAILED(hr = m_ProbeElement->AddHandler(m_ProbeReleasedEvent, m_ProbeReleasedHandler, TRUE)) ||
			FAILED(hr = m_ProbeElement->AddHandler(m_ProbeCanceledEvent, m_ProbeCanceledHandler, TRUE)) ||
			FAILED(hr = m_ProbeElement->AddHandler(m_ProbeCaptureLostEvent, m_ProbeCaptureLostHandler, TRUE)))
		{
			DetachPointerProbe();
			return hr;
		}

		m_ProbeStart = startHandle;
		LogToFile(STARTUP_LOG, L"Win11StartButton: pointer probe attached handle=%llu",
			(unsigned long long)startHandle);
		return S_OK;
	}

	HRESULT OnPointerProbe( StartPointerProbeEvent eventType,
		ABI::Windows::UI::Xaml::Input::IPointerRoutedEventArgs *args )
	{
		if (!args || !m_ProbeElement)
			return S_OK;

		CComPtr<ABI::Windows::UI::Input::IPointerPoint> point;
		HRESULT hr = args->GetCurrentPoint(m_ProbeElement, &point);
		if (FAILED(hr) || !point)
			return S_OK;

		UINT32 pointerId = 0;
		ABI::Windows::Foundation::Point position = {};
		boolean inContact = false;
		boolean handled = false;
		int deviceType = -1;
		boolean left = false, right = false, middle = false, barrel = false;

		point->get_PointerId(&pointerId);
		point->get_Position(&position);
		point->get_IsInContact(&inContact);
		args->get_Handled(&handled);

		CComPtr<ABI::Windows::Devices::Input::IPointerDevice> device;
		if (SUCCEEDED(point->get_PointerDevice(&device)) && device)
		{
			ABI::Windows::Devices::Input::PointerDeviceType type;
			if (SUCCEEDED(device->get_PointerDeviceType(&type)))
				deviceType = (int)type;
		}

		CComPtr<ABI::Windows::UI::Input::IPointerPointProperties> properties;
		if (SUCCEEDED(point->get_Properties(&properties)) && properties)
		{
			properties->get_IsLeftButtonPressed(&left);
			properties->get_IsRightButtonPressed(&right);
			properties->get_IsMiddleButtonPressed(&middle);
			properties->get_IsBarrelButtonPressed(&barrel);
		}

		const wchar_t *eventName = L"unknown";
		switch (eventType)
		{
			case START_POINTER_PRESSED: eventName = L"pressed"; break;
			case START_POINTER_RELEASED: eventName = L"released"; break;
			case START_POINTER_CANCELED: eventName = L"canceled"; break;
			case START_POINTER_CAPTURE_LOST: eventName = L"capture-lost"; break;
		}

		LogToFile(STARTUP_LOG,
			L"Win11StartButton: pointer probe %s device=%d id=%u handled=%d contact=%d pos=%.1f,%.1f buttons=%d/%d/%d barrel=%d",
			eventName, deviceType, pointerId, (int)handled, (int)inContact,
			position.X, position.Y, (int)left, (int)right, (int)middle, (int)barrel);
		return S_OK;
	}

	void ApplyState( bool enabled )
	{
		if (!m_Visual)
			return;

		std::vector<std::pair<InstanceHandle, StartElement>> elements;
		EnterCriticalSection(&m_Lock);
		for (auto it = m_Elements.begin(); it != m_Elements.end(); ++it)
			elements.push_back(*it);
		LeaveCriticalSection(&m_Lock);

		// Start and Task View share ExperienceToggleButton#LaunchListButton on
		// current Windows 11 builds. Resolve the attached AutomationId on the XAML
		// UI thread before changing any control or descendant.
		for (size_t i = 0; i < elements.size(); i++)
		{
			StartElement &record = elements[i].second;
			if (!IsStartControlCandidate(record) || record.startControlResolved)
				continue;

			bool isStartControl = false;
			if (SUCCEEDED(ResolveStartControl(elements[i].first, record, &isStartControl)))
			{
				record.startControlResolved = true;
				record.isStartControl = isStartControl;
				SetControlClassification(elements[i].first, isStartControl);
			}
		}

		InstanceHandle primaryStart = 0;
		EnterCriticalSection(&m_Lock);
		primaryStart = m_PrimaryStart;
		LeaveCriticalSection(&m_Lock);

		if (primaryStart)
		{
			HRESULT probeHr = AttachPointerProbe(primaryStart);
			if (FAILED(probeHr))
				LogToFile(STARTUP_LOG, L"Win11StartButton: pointer probe attach failed 0x%08X", probeHr);
		}
		else
		{
			DetachPointerProbe();
		}

		const bool allTaskbars = GetSettingBool(L"AllTaskbars");

		for (size_t i = 0; i < elements.size(); i++)
		{
			InstanceHandle handle = elements[i].first;
			StartElement record = elements[i].second;

			if (record.isStartControl)
			{
				// Research only: keep the real XAML Start control hit-testable so
				// the pointer probe can observe its routed input. The existing
				// WH_MOUSE hook still intercepts normal mouse input; holding F12
				// in this research build bypasses that hook for controlled tests.
				if (record.hitTestOverride)
				{
					HRESULT hr = ClearPropertyByName(handle, L"IsHitTestVisible");
					if (SUCCEEDED(hr))
					{
						bool value = false;
						SetOverrideFlags(handle, NULL, &value);
					}
				}
				continue;
			}

			if (!IsStartGlyph(record))
				continue;

			InstanceHandle startAncestor = GetStartAncestor(handle);
			if (!startAncestor)
				continue;
			bool target = allTaskbars || !primaryStart || startAncestor == primaryStart;

			if (enabled && target)
			{
				if (!record.visibilityOverride)
				{
					HRESULT hr = SetPropertyText(handle, L"Visibility", L"Collapsed");
					if (SUCCEEDED(hr))
					{
						bool value = true;
						SetOverrideFlags(handle, &value, NULL);
					}
				}
			}
			else if (record.visibilityOverride)
			{
				HRESULT hr = ClearPropertyByName(handle, L"Visibility");
				if (SUCCEEDED(hr))
				{
					bool value = false;
					SetOverrideFlags(handle, &value, NULL);
				}
			}
		}
	}

	LONG m_Refs;
	bool m_Advised;
	HWND m_Dispatch;
	CRITICAL_SECTION m_Lock;
	CComPtr<IUnknown> m_Site;
	CComPtr<IVisualTreeService> m_Visual;
	CComPtr<IXamlDiagnostics> m_Diagnostics;
	CComPtr<ABI::Windows::UI::Xaml::IUIElementStatics> m_UIElementStatics;
	InstanceHandle m_ProbeStart;
	CComPtr<ABI::Windows::UI::Xaml::IUIElement> m_ProbeElement;
	CComPtr<ABI::Windows::UI::Xaml::IRoutedEvent> m_ProbePressedEvent;
	CComPtr<ABI::Windows::UI::Xaml::IRoutedEvent> m_ProbeReleasedEvent;
	CComPtr<ABI::Windows::UI::Xaml::IRoutedEvent> m_ProbeCanceledEvent;
	CComPtr<ABI::Windows::UI::Xaml::IRoutedEvent> m_ProbeCaptureLostEvent;
	CComPtr<IInspectable> m_ProbePressedHandler;
	CComPtr<IInspectable> m_ProbeReleasedHandler;
	CComPtr<IInspectable> m_ProbeCanceledHandler;
	CComPtr<IInspectable> m_ProbeCaptureLostHandler;
	InstanceHandle m_PrimaryStart;
	unsigned int m_NextDiscoveryOrder;
	bool m_InjectionReferenceBalanced;
	std::unordered_map<InstanceHandle, StartElement> m_Elements;
};

STDMETHODIMP CStartPointerProbeHandler::Invoke( IInspectable *,
	ABI::Windows::UI::Xaml::Input::IPointerRoutedEventArgs *args )
{
	return m_Owner ? m_Owner->OnPointerProbe(m_EventType, args) : S_OK;
}

static CWin11StartButtonTap *GetTapRef( void )
{
	CWin11StartButtonTap *tap = NULL;
	AcquireSRWLockShared(&g_TapLock);
	tap = g_Tap;
	if (tap)
		tap->AddRef();
	ReleaseSRWLockShared(&g_TapLock);
	return tap;
}

class CStartButtonTapFactory: public IClassFactory
{
public:
	CStartButtonTapFactory( void ) { m_Refs = 1; }

	STDMETHODIMP QueryInterface( REFIID riid, void **ppv )
	{
		if (!ppv)
			return E_POINTER;
		*ppv = NULL;
		if (riid != IID_IUnknown && riid != IID_IClassFactory)
			return E_NOINTERFACE;
		*ppv = static_cast<IClassFactory*>(this);
		AddRef();
		return S_OK;
	}

	STDMETHODIMP_(ULONG) AddRef( void ) { return (ULONG)InterlockedIncrement(&m_Refs); }
	STDMETHODIMP_(ULONG) Release( void ) { return (ULONG)InterlockedDecrement(&m_Refs); }

	STDMETHODIMP CreateInstance( IUnknown *outer, REFIID riid, void **ppv )
	{
		if (!ppv)
			return E_POINTER;
		*ppv = NULL;
		if (outer)
			return CLASS_E_NOAGGREGATION;

		CWin11StartButtonTap *tap = new CWin11StartButtonTap();
		if (!tap)
			return E_OUTOFMEMORY;
		HRESULT hr = tap->QueryInterface(riid, ppv);
		tap->Release();
		return hr;
	}

	STDMETHODIMP LockServer( BOOL ) { return S_OK; }

private:
	LONG m_Refs;
};

static CStartButtonTapFactory g_Factory;

// xamlom.h declares DllGetClassObject through the platform headers, so export
// our implementation under that name via a linker alias.
extern "C" HRESULT STDMETHODCALLTYPE OpenShellStartButtonDllGetClassObject( REFCLSID clsid, REFIID riid, LPVOID *ppv )
{
	if (!IsEqualGUID(clsid, CLSID_OpenShellStartButtonTap))
		return CLASS_E_CLASSNOTAVAILABLE;
	return g_Factory.QueryInterface(riid, ppv);
}

#ifdef _M_IX86
#pragma comment(linker, "/EXPORT:DllGetClassObject=_OpenShellStartButtonDllGetClassObject@12,PRIVATE")
#else
#pragma comment(linker, "/EXPORT:DllGetClassObject=OpenShellStartButtonDllGetClassObject,PRIVATE")
#endif

typedef HRESULT (WINAPI *InitXamlDiagnosticsEx_t)( LPCWSTR, DWORD, LPCWSTR, LPCWSTR, CLSID, LPCWSTR );

struct ConnectAttempt
{
	InitXamlDiagnosticsEx_t init;
	const wchar_t *endpoint;
	wchar_t dllPath[MAX_PATH];
	HRESULT hr;
};

static DWORD WINAPI ConnectAttemptThread( LPVOID param )
{
	ConnectAttempt *attempt = (ConnectAttempt*)param;
	attempt->hr = attempt->init(attempt->endpoint, GetCurrentProcessId(), NULL,
		attempt->dllPath, CLSID_OpenShellStartButtonTap, NULL);
	return 0;
}

static DWORD WINAPI ConnectThread( LPVOID )
{
	HMODULE runtime = LoadLibraryEx(L"Windows.UI.Xaml.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
	if (!runtime)
	{
		InterlockedExchange(&g_ConnectStarted, 0);
		return 0;
	}

	InitXamlDiagnosticsEx_t init = (InitXamlDiagnosticsEx_t)GetProcAddress(runtime, "InitializeXamlDiagnosticsEx");
	if (!init)
	{
		FreeLibrary(runtime);
		InterlockedExchange(&g_ConnectStarted, 0);
		return 0;
	}

	HMODULE module = GetThisModule();
	wchar_t dllPath[MAX_PATH];
	if (!module || !GetModuleFileName(module, dllPath, _countof(dllPath)))
	{
		FreeLibrary(runtime);
		InterlockedExchange(&g_ConnectStarted, 0);
		return 0;
	}

	const wchar_t *endpoints[] = { L"VisualDiagConnection1", L"VisualDiagConnection2" };
	HRESULT last = E_FAIL;
	for (int retry = 0; retry < 8 && InterlockedCompareExchange(&g_StartButtonActive, 0, 0); retry++)
	{
		for (int i = 0; i < _countof(endpoints); i++)
		{
			ConnectAttempt attempt = {};
			attempt.init = init;
			attempt.endpoint = endpoints[i];
			Strcpy(attempt.dllPath, _countof(attempt.dllPath), dllPath);
			attempt.hr = E_FAIL;

			HANDLE thread = CreateThread(NULL, 0, ConnectAttemptThread, &attempt, 0, NULL);
			if (!thread)
				continue;
			WaitForSingleObject(thread, INFINITE);
			CloseHandle(thread);
			last = attempt.hr;
			if (SUCCEEDED(last))
			{
				LogToFile(STARTUP_LOG, L"Win11StartButton: connected using %s", endpoints[i]);
				FreeLibrary(runtime);
				return 0;
			}
		}
		Sleep(500);
	}

	LogToFile(STARTUP_LOG, L"Win11StartButton: connection failed 0x%08X", last);
	FreeLibrary(runtime);
	InterlockedExchange(&g_ConnectStarted, 0);
	return 0;
}

static void EnsureConnection( void )
{
	if (InterlockedCompareExchange(&g_ConnectStarted, 1, 0) != 0)
		return;

	HANDLE thread = CreateThread(NULL, 0, ConnectThread, NULL, 0, NULL);
	if (thread)
		CloseHandle(thread);
	else
		InterlockedExchange(&g_ConnectStarted, 0);
}

void StartWin11StartButtonMonitor( void )
{
	if (!IsWin11())
		return;

	InterlockedExchange(&g_StartButtonActive, 1);
	CWin11StartButtonTap *tap = GetTapRef();
	if (tap)
	{
		tap->RequestApply(false);
		tap->Release();
		return;
	}
	EnsureConnection();
}

void UpdateWin11StartButtonMonitor( void )
{
	if (!IsWin11())
		return;

	CWin11StartButtonTap *tap = GetTapRef();
	if (tap)
	{
		tap->RequestApply(false);
		tap->Release();
	}
	else if (InterlockedCompareExchange(&g_StartButtonActive, 0, 0))
	{
		EnsureConnection();
	}
}

void StopWin11StartButtonMonitor( void )
{
	if (!IsWin11())
		return;

	InterlockedExchange(&g_StartButtonActive, 0);
	CWin11StartButtonTap *tap = GetTapRef();
	if (tap)
	{
		tap->Shutdown();
		tap->Release();
	}
}
