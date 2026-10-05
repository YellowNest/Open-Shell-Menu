// Windows 11 native Start button suppression.
//
// Open-Shell's replacement button is a separate layered window. Windows 11
// renders its own Start button in XAML, so the old HWND hiding logic cannot
// remove the native glyph. This file uses the public XAML diagnostics API to
// suppress only the native Start glyph and its hit target. The XAML element is
// left in layout, so centered taskbar positioning remains owned by Windows.

#include "stdafx.h"
#include "Win11StartButtonTap.h"
#include "StartMenuHelper_h.h"
#include "dllmain.h"
#include "Settings.h"
#include "StringUtils.h"
#include "..\StartMenuDLL\LogManager.h"

#include <Windows.UI.Xaml.h>
#include <Windows.Devices.Input.h>
#include <Windows.UI.Xaml.Input.h>
#include <Windows.UI.Input.h>
#include <xamlom.h>
#include <ocidl.h>
#include <roapi.h>
#include <winstring.h>
#include <wrl.h>
#include <wrl/client.h>
#include <wrl/implements.h>
#include <unordered_map>
#include <vector>

static const GUID CLSID_OpenShellStartButtonTap =
{ 0x7d15741f, 0x2f3b, 0x4971, { 0xb8, 0x91, 0x6a, 0x5d, 0x42, 0xd7, 0x1a, 0x34 } };

static const UINT WM_OS_STARTBUTTON_APPLY = WM_APP + 0x35B;
static const UINT WM_OS_STARTBUTTON_REPROBE = WM_APP + 0x35C;

enum
{
	WIN11_START_INPUT_FALLBACK,
	WIN11_START_INPUT_PROBING,
	WIN11_START_INPUT_ACTIVE,
};

static UINT GetWin11StartInputMessage( void )
{
	static UINT message = RegisterWindowMessage(L"OpenShell.Win11StartInput");
	return message;
}

static UINT GetWin11StartInputStateMessage( void )
{
	static UINT message = RegisterWindowMessage(L"OpenShell.Win11StartInputState");
	return message;
}

static bool IsTaskbarWindow( HWND hwnd )
{
	if (!hwnd)
		return false;
	wchar_t className[64] = {};
	if (!GetClassName(hwnd, className, _countof(className)))
		return false;
	return wcscmp(className, L"Shell_TrayWnd") == 0 ||
		wcscmp(className, L"Shell_SecondaryTrayWnd") == 0;
}

struct TaskbarPointSearch
{
	POINT point;
	HWND taskbar;
};

static BOOL CALLBACK FindTaskbarAtPointProc( HWND hwnd, LPARAM lParam )
{
	TaskbarPointSearch *search = (TaskbarPointSearch*)lParam;
	if (!search || !IsTaskbarWindow(hwnd))
		return TRUE;

	RECT rect = {};
	if (GetWindowRect(hwnd, &rect) && PtInRect(&rect, search->point))
	{
		search->taskbar = hwnd;
		return FALSE;
	}
	return TRUE;
}

static HWND FindTaskbarAtPoint( POINT point )
{
	TaskbarPointSearch search = { point, NULL };
	EnumWindows(FindTaskbarAtPointProc, (LPARAM)&search);
	return search.taskbar;
}

static volatile LONG g_StartButtonActive = 0;
static volatile LONG g_StartButtonEnabled = 0;
static volatile LONG g_AllTaskbars = 0;
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

struct StartInputRoute
{
	InstanceHandle startHandle;
	InstanceHandle rootHandle;
	CComPtr<ABI::Windows::UI::Xaml::IUIElement> startElement;
	CComPtr<ABI::Windows::UI::Xaml::IFrameworkElement> startFramework;
	CComPtr<ABI::Windows::UI::Xaml::IUIElement> rootElement;
	CComPtr<IInspectable> handler;
	bool verified;
	bool pointerInside;
	HWND taskbar;
	DWORD lastPressTime;
	POINT lastPressPoint;
	UINT lastPressMessage;

	StartInputRoute( void )
	{
		startHandle = 0;
		rootHandle = 0;
		verified = false;
		pointerInside = false;
		taskbar = NULL;
		lastPressTime = 0;
		lastPressPoint.x = 0;
		lastPressPoint.y = 0;
		lastPressMessage = 0;
	}
};

class CWin11StartButtonTap;

class CStartPointerHandler:
	public Microsoft::WRL::RuntimeClass<
		Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::WinRtClassicComMix>,
		ABI::Windows::UI::Xaml::Input::IPointerEventHandler>
{
public:
	CStartPointerHandler( CWin11StartButtonTap *owner, InstanceHandle startHandle );
	~CStartPointerHandler( void );

	HRESULT STDMETHODCALLTYPE GetTrustLevel( TrustLevel *trustLevel ) override
	{
		if (!trustLevel)
			return E_POINTER;
		*trustLevel = BaseTrust;
		return S_OK;
	}

	HRESULT STDMETHODCALLTYPE Invoke( IInspectable *sender,
		ABI::Windows::UI::Xaml::Input::IPointerRoutedEventArgs *args ) override;

private:
	CWin11StartButtonTap *m_Owner;
	InstanceHandle m_StartHandle;
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
		m_NextDiscoveryOrder = 0;
		m_ExpectedInputRoutes = 0;
		m_LastInputState = -1;
		InitializeCriticalSection(&m_Lock);
		_AtlModule.Lock();
	}

	~CWin11StartButtonTap( void )
	{
		if (m_Dispatch && GetWindowThreadProcessId(m_Dispatch, NULL) == GetCurrentThreadId())
			DetachAllInputRoutes();
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
		_AtlModule.Unlock();
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
			HRESULT hr = m_Visual->UnadviseVisualTreeChange(static_cast<IVisualTreeServiceCallback*>(this));
			if (FAILED(hr))
			{
				LogToFile(STARTUP_LOG, L"Win11StartButtonTap: visual tree unadvise failed 0x%08X", hr);
				return hr;
			}
			m_Advised = false;
		}
		DetachAllInputRoutes();
		m_UIElementStatics.Release();
		m_PointerEnteredEvent.Release();
		m_PointerMovedEvent.Release();
		m_PointerPressedEvent.Release();
		m_PointerReleasedEvent.Release();
		m_PointerExitedEvent.Release();
		m_Diagnostics.Release();
		m_Visual.Release();
		m_Site.Release();
		m_LastInputState = -1;

		if (!site)
		{
			AcquireSRWLockExclusive(&g_TapLock);
			if (g_Tap == this)
				g_Tap = NULL;
			ReleaseSRWLockExclusive(&g_TapLock);

			EnterCriticalSection(&m_Lock);
			m_Elements.clear();
			m_PrimaryStart = 0;
			m_NextDiscoveryOrder = 0;
			LeaveCriticalSection(&m_Lock);

			if (m_Dispatch && GetWindowThreadProcessId(m_Dispatch, NULL) == GetCurrentThreadId())
			{
				HWND dispatch = m_Dispatch;
				m_Dispatch = NULL;
				SetWindowLongPtr(dispatch, GWLP_USERDATA, 0);
				DestroyWindow(dispatch);
			}

			InterlockedExchange(&g_ConnectStarted, 0);
			return S_OK;
		}

		EnterCriticalSection(&m_Lock);
		m_Elements.clear();
		m_PrimaryStart = 0;
		m_NextDiscoveryOrder = 0;
		LeaveCriticalSection(&m_Lock);

		m_Site = site;

		// XAML Diagnostics keeps the TAP site object and its module loaded for
		// the lifetime of the diagnostics session. Do not reject a late SetSite
		// when Open-Shell is inactive: keeping the site attached lets a later
		// StartMenuDLL instance reuse the same resident TAP safely.
		HRESULT hr = site->QueryInterface(__uuidof(IVisualTreeService), (void**)&m_Visual);
		if (FAILED(hr) || !m_Visual)
		{
			m_Diagnostics.Release();
			m_Visual.Release();
			m_Site.Release();
			InterlockedExchange(&g_ConnectStarted, 0);
			return hr;
		}

		HRESULT diagnosticsHr = site->QueryInterface(__uuidof(IXamlDiagnostics), (void**)&m_Diagnostics);
		if (FAILED(diagnosticsHr))
		{
			m_Diagnostics.Release();
			LogToFile(STARTUP_LOG, L"Win11StartInput: IXamlDiagnostics unavailable 0x%08X; keeping WH_MOUSE fallback", diagnosticsHr);
		}

		if (!CreateDispatchWindow())
		{
			DWORD error = GetLastError();
			m_Diagnostics.Release();
			m_Visual.Release();
			m_Site.Release();
			InterlockedExchange(&g_ConnectStarted, 0);
			return HRESULT_FROM_WIN32(error);
		}

		// Advise replays the existing tree. OnVisualTreeChange only records
		// element handles; all property access is dispatched afterwards.
		hr = m_Visual->AdviseVisualTreeChange(static_cast<IVisualTreeServiceCallback*>(this));
		if (SUCCEEDED(hr))
		{
			m_Advised = true;

			// Publish the TAP only after the subscription is fully established.
			// Otherwise an Advise failure can leave g_Tap pointing at an object
			// that the XAML runtime is about to release.
			AcquireSRWLockExclusive(&g_TapLock);
			g_Tap = this;
			ReleaseSRWLockExclusive(&g_TapLock);

			RequestApply(false);
		}
		else
		{
			if (m_Dispatch)
			{
				HWND dispatch = m_Dispatch;
				m_Dispatch = NULL;
				SetWindowLongPtr(dispatch, GWLP_USERDATA, 0);
				DestroyWindow(dispatch);
			}
			m_Diagnostics.Release();
			m_Visual.Release();
			m_Site.Release();
			InterlockedExchange(&g_ConnectStarted, 0);
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
		// VisualElement is an [in] parameter. The XAML diagnostics runtime owns
		// the BSTR fields; copy the values we need but never free callback input.
		bool interesting = false;
		bool routeTopologyChanged = false;

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
			const bool isStartCandidate = IsStartControlCandidate(record);

			routeTopologyChanged = isStartCandidate;
			interesting = routeTopologyChanged || IsStartGlyph(record) ||
				IsUnderStartButtonLocked(record.parent);
		}
		LeaveCriticalSection(&m_Lock);

		// Do not mutate XAML routed-event handler collections from inside the
		// visual-tree callback. Any topology change that can alter the required
		// Start routes must first re-arm the WH_MOUSE fallback. This closes the
		// window between a new taskbar/XAML source appearing and the asynchronous
		// ApplyState pass recalculating whether every native Start slot is routed.
		if (mutationType == Add && routeTopologyChanged &&
			InterlockedCompareExchange(&g_StartButtonEnabled, 0, 0) && !m_InputRoutes.empty())
		{
			ReportInputBridgeState(WIN11_START_INPUT_PROBING);
		}

		// If an active route is disappearing, queue the fallback transition first;
		// the posted apply pass then removes the stale route after the diagnostics
		// callback has unwound.
		if (mutationType == Remove && InputRouteUsesHandle(element.Handle))
		{
			ReportInputBridgeState(WIN11_START_INPUT_PROBING);
			interesting = true;
		}
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

	void RequestInputBridgeReprobe( void )
	{
		if (m_Dispatch)
			PostMessage(m_Dispatch, WM_OS_STARTBUTTON_REPROBE, 0, 0);
	}

	HRESULT OnStartPointer( InstanceHandle startHandle,
		ABI::Windows::UI::Xaml::Input::IPointerRoutedEventArgs *args )
	{
		if (!args || !InterlockedCompareExchange(&g_StartButtonActive, 0, 0) ||
			!InterlockedCompareExchange(&g_StartButtonEnabled, 0, 0))
			return S_OK;

		StartInputRoute *route = FindInputRoute(startHandle);
		if (!route)
			return S_OK;

		CComPtr<ABI::Windows::UI::Xaml::Input::IPointer> pointer;
		HRESULT hr = args->get_Pointer(&pointer);
		if (FAILED(hr) || !pointer)
		{
			InvalidateInputRoute(*route);
			return S_OK;
		}

		ABI::Windows::Devices::Input::PointerDeviceType deviceType =
			ABI::Windows::Devices::Input::PointerDeviceType_Touch;
		hr = pointer->get_PointerDeviceType(&deviceType);
		if (FAILED(hr))
		{
			InvalidateInputRoute(*route);
			return S_OK;
		}
		if (deviceType != ABI::Windows::Devices::Input::PointerDeviceType_Mouse)
			return S_OK;

		// This bridge currently routes mouse input only. GetCursorPos is sufficient
		// for the screen-space mouse location and, unlike GetPointerInfo, does not
		// add a Windows 8+ User32 import to StartMenuHelper.
		POINT screenPoint = {};
		if (!GetCursorPos(&screenPoint))
		{
			InvalidateInputRoute(*route);
			return S_OK;
		}

		CComPtr<ABI::Windows::UI::Input::IPointerPoint> point;
		hr = args->GetCurrentPoint(route->startElement, &point);
		if (FAILED(hr) || !point)
		{
			InvalidateInputRoute(*route);
			return S_OK;
		}

		ABI::Windows::Foundation::Point position = {};
		DOUBLE width = 0;
		DOUBLE height = 0;
		if (FAILED(point->get_Position(&position)) ||
			FAILED(route->startFramework->get_ActualWidth(&width)) ||
			FAILED(route->startFramework->get_ActualHeight(&height)))
		{
			InvalidateInputRoute(*route);
			return S_OK;
		}

		bool inside = position.X >= 0 && position.Y >= 0 &&
			position.X < width && position.Y < height;
		if (!inside)
		{
			if (route->pointerInside && route->taskbar)
				PostInputMessage(route->taskbar, WM_MOUSELEAVE, screenPoint);
			route->pointerInside = false;
			return S_OK;
		}

		HWND taskbar = FindTaskbarAtPoint(screenPoint);
		if (!taskbar || !IsWindow(taskbar))
		{
			LogToFile(STARTUP_LOG, L"Win11StartInput: no taskbar at pointer position %d,%d", screenPoint.x, screenPoint.y);
			InvalidateInputRoute(*route);
			return S_OK;
		}

		UINT mouseMessage = WM_MOUSEMOVE;
		CComPtr<ABI::Windows::UI::Input::IPointerPointProperties> properties;
		if (SUCCEEDED(point->get_Properties(&properties)) && properties)
		{
			ABI::Windows::UI::Input::PointerUpdateKind updateKind =
				ABI::Windows::UI::Input::PointerUpdateKind_Other;
			if (SUCCEEDED(properties->get_PointerUpdateKind(&updateKind)))
			{
				switch (updateKind)
				{
				case ABI::Windows::UI::Input::PointerUpdateKind_LeftButtonPressed:
					mouseMessage = WM_LBUTTONDOWN;
					break;
				case ABI::Windows::UI::Input::PointerUpdateKind_LeftButtonReleased:
					mouseMessage = WM_LBUTTONUP;
					break;
				case ABI::Windows::UI::Input::PointerUpdateKind_RightButtonPressed:
					mouseMessage = WM_RBUTTONDOWN;
					break;
				case ABI::Windows::UI::Input::PointerUpdateKind_RightButtonReleased:
					mouseMessage = WM_RBUTTONUP;
					break;
				case ABI::Windows::UI::Input::PointerUpdateKind_MiddleButtonPressed:
					mouseMessage = WM_MBUTTONDOWN;
					break;
				case ABI::Windows::UI::Input::PointerUpdateKind_MiddleButtonReleased:
					mouseMessage = WM_MBUTTONUP;
					break;
				default:
					break;
				}
			}
		}

		// WH_MOUSE used to deliver Windows-generated double-click messages.
		// Routed pointer events expose button transitions instead, so preserve the
		// same behavior before retiring the hook.
		UINT doubleClickMessage = 0;
		if (mouseMessage == WM_LBUTTONDOWN)
			doubleClickMessage = WM_LBUTTONDBLCLK;
		else if (mouseMessage == WM_RBUTTONDOWN)
			doubleClickMessage = WM_RBUTTONDBLCLK;
		else if (mouseMessage == WM_MBUTTONDOWN)
			doubleClickMessage = WM_MBUTTONDBLCLK;

		if (doubleClickMessage)
		{
			DWORD now = GetTickCount();
			LONG dx = screenPoint.x - route->lastPressPoint.x;
			LONG dy = screenPoint.y - route->lastPressPoint.y;
			if (dx < 0) dx = -dx;
			if (dy < 0) dy = -dy;
			const int maxDx = GetSystemMetrics(SM_CXDOUBLECLK) / 2;
			const int maxDy = GetSystemMetrics(SM_CYDOUBLECLK) / 2;

			if (route->lastPressMessage == mouseMessage &&
				now - route->lastPressTime <= GetDoubleClickTime() &&
				dx <= maxDx && dy <= maxDy)
			{
				mouseMessage = doubleClickMessage;
				route->lastPressTime = 0;
				route->lastPressMessage = 0;
			}
			else
			{
				route->lastPressTime = now;
				route->lastPressPoint = screenPoint;
				route->lastPressMessage = mouseMessage;
			}
		}

		// Prove that XAML accepts suppression before forwarding the event and
		// before this route can retire WH_MOUSE. If this ever fails, prefer a
		// single swallowed event plus restored fallback over double-activating
		// both Open-Shell and the native Start button.
		hr = args->put_Handled(TRUE);
		if (FAILED(hr))
		{
			LogToFile(STARTUP_LOG, L"Win11StartInput: failed to mark routed pointer handled 0x%08X", hr);
			InvalidateInputRoute(*route);
			return S_OK;
		}

		if (!PostInputMessage(taskbar, mouseMessage, screenPoint))
		{
			InvalidateInputRoute(*route);
			return S_OK;
		}

		route->pointerInside = true;
		route->taskbar = taskbar;

		if (!route->verified)
		{
			route->verified = true;
			EvaluateInputBridgeState();
		}
		return S_OK;
	}

	HRESULT Deactivate( void )
	{
		// The diagnostics runtime retains this site beyond Open-Shell's lifetime.
		// Restore only our overrides; keep the site, callback and dispatch window
		// alive so StartMenuDLL can unload/reload independently.
		if (!m_Dispatch)
			return E_UNEXPECTED;
		RequestApply(true);
		return S_OK;
	}

private:
	bool PostInputMessage( HWND taskbar, UINT mouseMessage, POINT screenPoint )
	{
		if (!taskbar || !IsWindow(taskbar))
			return false;

		LPARAM position = 0;
		if (mouseMessage != WM_MOUSELEAVE)
		{
			POINT taskbarPoint = screenPoint;
			if (!ScreenToClient(taskbar, &taskbarPoint))
				return false;
			position = MAKELPARAM(taskbarPoint.x, taskbarPoint.y);
		}
		return PostMessage(taskbar, GetWin11StartInputMessage(), mouseMessage, position) != FALSE;
	}

	void ReprobeInputBridgeState( void )
	{
		for (size_t i = 0; i < m_InputRoutes.size(); i++)
			m_InputRoutes[i].verified = false;
		m_LastInputState = -1;
		ReportInputBridgeState(
			InterlockedCompareExchange(&g_StartButtonEnabled, 0, 0) ?
			WIN11_START_INPUT_PROBING : WIN11_START_INPUT_FALLBACK);
	}

	void ReportInputBridgeState( LONG state )
	{
		if (!InterlockedCompareExchange(&g_StartButtonActive, 0, 0))
		{
			m_LastInputState = -1;
			return;
		}
		if (m_LastInputState == state)
			return;

		HWND taskbar = FindWindow(L"Shell_TrayWnd", NULL);
		if (taskbar && PostMessage(taskbar, GetWin11StartInputStateMessage(), state, 0))
		{
			m_LastInputState = state;
			LogToFile(STARTUP_LOG, L"Win11StartInput: routing state %d", state);
		}
	}

	StartInputRoute *FindInputRoute( InstanceHandle startHandle )
	{
		for (size_t i = 0; i < m_InputRoutes.size(); i++)
			if (m_InputRoutes[i].startHandle == startHandle)
				return &m_InputRoutes[i];
		return NULL;
	}

	bool InputRouteUsesHandle( InstanceHandle handle ) const
	{
		for (size_t i = 0; i < m_InputRoutes.size(); i++)
			if (m_InputRoutes[i].startHandle == handle ||
				m_InputRoutes[i].rootHandle == handle)
				return true;
		return false;
	}

	void InvalidateInputRoute( StartInputRoute &route )
	{
		route.verified = false;
		ReportInputBridgeState(WIN11_START_INPUT_PROBING);
	}

	bool EnsureUIElementStatics( void )
	{
		if (m_UIElementStatics && m_PointerEnteredEvent && m_PointerMovedEvent &&
			m_PointerPressedEvent && m_PointerReleasedEvent && m_PointerExitedEvent)
			return true;

		// Keep StartMenuHelper loadable on pre-Windows-8 systems. These WinRT
		// entry points are resolved only when the Windows 11 XAML path is active,
		// rather than becoming static DLL imports.
		typedef HRESULT (WINAPI *WindowsCreateString_t)( PCNZWCH, UINT32, HSTRING* );
		typedef HRESULT (WINAPI *WindowsDeleteString_t)( HSTRING );
		typedef HRESULT (WINAPI *RoGetActivationFactory_t)( HSTRING, REFIID, void** );

		HMODULE combase = GetModuleHandle(L"combase.dll");
		if (!combase)
		{
			LogToFile(STARTUP_LOG, L"Win11StartInput: combase is unavailable; keeping WH_MOUSE fallback");
			return false;
		}

		WindowsCreateString_t createString =
			(WindowsCreateString_t)GetProcAddress(combase, "WindowsCreateString");
		WindowsDeleteString_t deleteString =
			(WindowsDeleteString_t)GetProcAddress(combase, "WindowsDeleteString");
		RoGetActivationFactory_t getActivationFactory =
			(RoGetActivationFactory_t)GetProcAddress(combase, "RoGetActivationFactory");
		if (!createString || !deleteString || !getActivationFactory)
		{
			LogToFile(STARTUP_LOG, L"Win11StartInput: WinRT activation exports are unavailable; keeping WH_MOUSE fallback");
			return false;
		}

		static const wchar_t CLASS_NAME[] = L"Windows.UI.Xaml.UIElement";
		HSTRING className = NULL;
		HRESULT hr = createString(CLASS_NAME, _countof(CLASS_NAME) - 1, &className);
		if (FAILED(hr))
			return false;

		hr = getActivationFactory(className, __uuidof(ABI::Windows::UI::Xaml::IUIElementStatics),
			(void**)&m_UIElementStatics);
		deleteString(className);
		if (FAILED(hr) || !m_UIElementStatics)
		{
			LogToFile(STARTUP_LOG, L"Win11StartInput: UIElement statics unavailable 0x%08X", hr);
			return false;
		}

		if (FAILED(m_UIElementStatics->get_PointerEnteredEvent(&m_PointerEnteredEvent)) ||
			FAILED(m_UIElementStatics->get_PointerMovedEvent(&m_PointerMovedEvent)) ||
			FAILED(m_UIElementStatics->get_PointerPressedEvent(&m_PointerPressedEvent)) ||
			FAILED(m_UIElementStatics->get_PointerReleasedEvent(&m_PointerReleasedEvent)) ||
			FAILED(m_UIElementStatics->get_PointerExitedEvent(&m_PointerExitedEvent)))
		{
			LogToFile(STARTUP_LOG, L"Win11StartInput: pointer routed-event metadata unavailable");
			m_PointerEnteredEvent.Release();
			m_PointerMovedEvent.Release();
			m_PointerPressedEvent.Release();
			m_PointerReleasedEvent.Release();
			m_PointerExitedEvent.Release();
			m_UIElementStatics.Release();
			return false;
		}
		return true;
	}

	bool GetInputRoot( InstanceHandle startHandle, InstanceHandle *rootHandle,
		CComPtr<ABI::Windows::UI::Xaml::IUIElement> &rootElement )
	{
		if (!m_Diagnostics)
			return false;

		std::vector<InstanceHandle> ancestors;
		EnterCriticalSection(&m_Lock);
		auto it = m_Elements.find(startHandle);
		InstanceHandle current = it != m_Elements.end() ? it->second.parent : 0;
		for (int depth = 0; depth < 64 && current; depth++)
		{
			ancestors.push_back(current);
			auto parent = m_Elements.find(current);
			if (parent == m_Elements.end())
				break;
			current = parent->second.parent;
		}
		LeaveCriticalSection(&m_Lock);

		for (auto it2 = ancestors.rbegin(); it2 != ancestors.rend(); ++it2)
		{
			CComPtr<IInspectable> inspectable;
			if (FAILED(m_Diagnostics->GetIInspectableFromHandle(*it2, &inspectable)) || !inspectable)
				continue;

			CComPtr<ABI::Windows::UI::Xaml::IUIElement> element;
			if (SUCCEEDED(inspectable->QueryInterface(__uuidof(ABI::Windows::UI::Xaml::IUIElement),
				(void**)&element)) && element)
			{
				*rootHandle = *it2;
				rootElement = element;
				return true;
			}
		}
		return false;
	}

	bool AttachInputRoute( InstanceHandle startHandle )
	{
		if (FindInputRoute(startHandle))
			return true;
		if (!m_Diagnostics || !EnsureUIElementStatics())
			return false;

		CComPtr<IInspectable> inspectable;
		if (FAILED(m_Diagnostics->GetIInspectableFromHandle(startHandle, &inspectable)) || !inspectable)
			return false;

		StartInputRoute route;
		route.startHandle = startHandle;
		if (FAILED(inspectable->QueryInterface(__uuidof(ABI::Windows::UI::Xaml::IUIElement),
			(void**)&route.startElement)) || !route.startElement)
			return false;
		if (FAILED(inspectable->QueryInterface(__uuidof(ABI::Windows::UI::Xaml::IFrameworkElement),
			(void**)&route.startFramework)) || !route.startFramework)
			return false;
		if (!GetInputRoot(startHandle, &route.rootHandle, route.rootElement))
			return false;

		auto handler = Microsoft::WRL::Make<CStartPointerHandler>(this, startHandle);
		if (!handler)
			return false;
		HRESULT hr = handler->QueryInterface(IID_PPV_ARGS(&route.handler));
		if (FAILED(hr) || !route.handler)
			return false;

		hr = route.rootElement->AddHandler(m_PointerEnteredEvent, route.handler, TRUE);
		if (SUCCEEDED(hr))
			hr = route.rootElement->AddHandler(m_PointerMovedEvent, route.handler, TRUE);
		if (SUCCEEDED(hr))
			hr = route.rootElement->AddHandler(m_PointerPressedEvent, route.handler, TRUE);
		if (SUCCEEDED(hr))
			hr = route.rootElement->AddHandler(m_PointerReleasedEvent, route.handler, TRUE);
		if (SUCCEEDED(hr))
			hr = route.rootElement->AddHandler(m_PointerExitedEvent, route.handler, TRUE);
		if (FAILED(hr))
		{
			route.rootElement->RemoveHandler(m_PointerEnteredEvent, route.handler);
			route.rootElement->RemoveHandler(m_PointerMovedEvent, route.handler);
			route.rootElement->RemoveHandler(m_PointerPressedEvent, route.handler);
			route.rootElement->RemoveHandler(m_PointerReleasedEvent, route.handler);
			route.rootElement->RemoveHandler(m_PointerExitedEvent, route.handler);
			LogToFile(STARTUP_LOG, L"Win11StartInput: routed handler attach failed 0x%08X", hr);
			return false;
		}

		m_InputRoutes.push_back(route);
		LogToFile(STARTUP_LOG, L"Win11StartInput: route attached for handle %llu via root %llu",
			(unsigned long long)startHandle, (unsigned long long)route.rootHandle);
		return true;
	}

	void DetachInputRoute( size_t index )
	{
		if (index >= m_InputRoutes.size())
			return;
		StartInputRoute &route = m_InputRoutes[index];

		if (route.pointerInside && route.taskbar)
		{
			POINT point = {};
			GetCursorPos(&point);
			PostInputMessage(route.taskbar, WM_MOUSELEAVE, point);
		}

		if (route.rootElement && route.handler)
		{
			if (m_PointerEnteredEvent) route.rootElement->RemoveHandler(m_PointerEnteredEvent, route.handler);
			if (m_PointerMovedEvent) route.rootElement->RemoveHandler(m_PointerMovedEvent, route.handler);
			if (m_PointerPressedEvent) route.rootElement->RemoveHandler(m_PointerPressedEvent, route.handler);
			if (m_PointerReleasedEvent) route.rootElement->RemoveHandler(m_PointerReleasedEvent, route.handler);
			if (m_PointerExitedEvent) route.rootElement->RemoveHandler(m_PointerExitedEvent, route.handler);
		}
		m_InputRoutes.erase(m_InputRoutes.begin() + index);
	}

	void DetachAllInputRoutes( void )
	{
		while (!m_InputRoutes.empty())
			DetachInputRoute(m_InputRoutes.size() - 1);
		m_ExpectedInputRoutes = 0;
	}

	void SyncInputRoutes( const std::vector<InstanceHandle> &targets )
	{
		m_ExpectedInputRoutes = targets.size();

		for (size_t i = 0; i < m_InputRoutes.size();)
		{
			bool keep = false;
			for (size_t j = 0; j < targets.size(); j++)
				if (targets[j] == m_InputRoutes[i].startHandle)
				{
					keep = true;
					break;
				}

			// Visual-tree removals are applied after the diagnostics callback
			// unwinds. A root can disappear before its Start descendant is reported
			// removed, so validate both handles before retaining the route.
			if (keep)
			{
				EnterCriticalSection(&m_Lock);
				keep = m_Elements.find(m_InputRoutes[i].startHandle) != m_Elements.end() &&
					m_Elements.find(m_InputRoutes[i].rootHandle) != m_Elements.end();
				LeaveCriticalSection(&m_Lock);
			}

			if (!keep)
				DetachInputRoute(i);
			else
				i++;
		}

		for (size_t i = 0; i < targets.size(); i++)
			if (!FindInputRoute(targets[i]))
				AttachInputRoute(targets[i]);
	}

	void EvaluateInputBridgeState( void )
	{
		if (!InterlockedCompareExchange(&g_StartButtonEnabled, 0, 0) ||
			!m_ExpectedInputRoutes || m_InputRoutes.size() != m_ExpectedInputRoutes)
		{
			ReportInputBridgeState(WIN11_START_INPUT_FALLBACK);
			return;
		}

		bool verified = true;
		EnterCriticalSection(&m_Lock);
		for (size_t i = 0; i < m_InputRoutes.size(); i++)
		{
			auto element = m_Elements.find(m_InputRoutes[i].startHandle);
			if (element == m_Elements.end() || !element->second.hitTestOverride)
			{
				verified = false;
				break;
			}
			if (!m_InputRoutes[i].verified)
				verified = false;
		}
		LeaveCriticalSection(&m_Lock);

		ReportInputBridgeState(verified ? WIN11_START_INPUT_ACTIVE : WIN11_START_INPUT_PROBING);
	}

	static LRESULT CALLBACK DispatchProc( HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam )
	{
		CWin11StartButtonTap *tap = (CWin11StartButtonTap*)GetWindowLongPtr(hwnd, GWLP_USERDATA);
		if (msg == WM_NCCREATE)
		{
			CREATESTRUCT *create = (CREATESTRUCT*)lParam;
			tap = (CWin11StartButtonTap*)create->lpCreateParams;
			SetWindowLongPtr(hwnd, GWLP_USERDATA, (LONG_PTR)tap);
		}
		if (msg == WM_OS_STARTBUTTON_REPROBE && tap)
		{
			tap->AddRef();
			tap->ReprobeInputBridgeState();
			tap->Release();
			return 0;
		}
		if (msg == WM_OS_STARTBUTTON_APPLY && tap)
		{
			// GWLP_USERDATA is a raw pointer. Hold the TAP alive while ApplyState
			// may detach the last routed-event handler that also references it.
			tap->AddRef();
			bool enabled = InterlockedCompareExchange(&g_StartButtonActive, 0, 0) != 0 &&
				InterlockedCompareExchange(&g_StartButtonEnabled, 0, 0) != 0;
			tap->ApplyState(enabled);
			tap->Release();
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

		const bool allTaskbars = InterlockedCompareExchange(&g_AllTaskbars, 0, 0) != 0;

		size_t startControlCount = 0;
		for (size_t i = 0; i < elements.size(); i++)
			if (elements[i].second.isStartControl)
				startControlCount++;

		// The fallback hook is thread-global. It can be retired only when every
		// native Start slot has an Open-Shell replacement/input route. With
		// multiple taskbars and AllTaskbars disabled, secondary native Start
		// controls must remain hit-testable for touch/pen and keep the established
		// WH_MOUSE fallback for mouse input.
		const bool canRetireMouseHook = allTaskbars || startControlCount <= 1;
		std::vector<InstanceHandle> inputTargets;
		if (enabled && canRetireMouseHook)
		{
			for (size_t i = 0; i < elements.size(); i++)
			{
				if (!elements[i].second.isStartControl)
					continue;
				InstanceHandle handle = elements[i].first;
				if (allTaskbars || !primaryStart || handle == primaryStart)
					inputTargets.push_back(handle);
			}
		}
		SyncInputRoutes(inputTargets);

		for (size_t i = 0; i < elements.size(); i++)
		{
			InstanceHandle handle = elements[i].first;
			StartElement record = elements[i].second;

			if (record.isStartControl)
			{
				bool target = allTaskbars || !primaryStart || handle == primaryStart;
				if (enabled && target)
				{
					if (!record.hitTestOverride)
					{
						HRESULT hr = SetPropertyText(handle, L"IsHitTestVisible", L"False");
						if (SUCCEEDED(hr))
						{
							bool value = true;
							SetOverrideFlags(handle, NULL, &value);
						}
					}
				}
				else if (record.hitTestOverride)
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

		EvaluateInputBridgeState();
	}

	LONG m_Refs;
	bool m_Advised;
	HWND m_Dispatch;
	CRITICAL_SECTION m_Lock;
	CComPtr<IUnknown> m_Site;
	CComPtr<IVisualTreeService> m_Visual;
	CComPtr<IXamlDiagnostics> m_Diagnostics;
	CComPtr<ABI::Windows::UI::Xaml::IUIElementStatics> m_UIElementStatics;
	CComPtr<ABI::Windows::UI::Xaml::IRoutedEvent> m_PointerEnteredEvent;
	CComPtr<ABI::Windows::UI::Xaml::IRoutedEvent> m_PointerMovedEvent;
	CComPtr<ABI::Windows::UI::Xaml::IRoutedEvent> m_PointerPressedEvent;
	CComPtr<ABI::Windows::UI::Xaml::IRoutedEvent> m_PointerReleasedEvent;
	CComPtr<ABI::Windows::UI::Xaml::IRoutedEvent> m_PointerExitedEvent;
	InstanceHandle m_PrimaryStart;
	unsigned int m_NextDiscoveryOrder;
	size_t m_ExpectedInputRoutes;
	LONG m_LastInputState;
	std::unordered_map<InstanceHandle, StartElement> m_Elements;
	std::vector<StartInputRoute> m_InputRoutes;
};

CStartPointerHandler::CStartPointerHandler( CWin11StartButtonTap *owner, InstanceHandle startHandle )
	: m_Owner(owner), m_StartHandle(startHandle)
{
	if (m_Owner)
		m_Owner->AddRef();
}

CStartPointerHandler::~CStartPointerHandler( void )
{
	if (m_Owner)
		m_Owner->Release();
}

HRESULT STDMETHODCALLTYPE CStartPointerHandler::Invoke( IInspectable *,
	ABI::Windows::UI::Xaml::Input::IPointerRoutedEventArgs *args )
{
	return m_Owner ? m_Owner->OnStartPointer(m_StartHandle, args) : S_OK;
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

	STDMETHODIMP LockServer( BOOL lock )
	{
		if (lock)
			_AtlModule.Lock();
		else
			_AtlModule.Unlock();
		return S_OK;
	}

private:
	LONG m_Refs;
};

static CStartButtonTapFactory g_Factory;

HRESULT GetWin11StartButtonTapClassObject( REFCLSID clsid, REFIID riid, LPVOID *ppv )
{
	if (!IsEqualGUID(clsid, CLSID_OpenShellStartButtonTap))
		return CLASS_E_CLASSNOTAVAILABLE;
	return g_Factory.QueryInterface(riid, ppv);
}

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

static DWORD FinishConnectThread( HMODULE moduleReference, HMODULE runtime, bool resetConnectionState )
{
	if (runtime)
		FreeLibrary(runtime);
	if (resetConnectionState)
		InterlockedExchange(&g_ConnectStarted, 0);

	// Keep a private StartMenuHelper reference while this worker is running.
	// Release it atomically with thread termination so a failed diagnostics
	// connection cannot unload the helper underneath the worker's return path.
	FreeLibraryAndExitThread(moduleReference, 0);
	return 0;
}

static DWORD WINAPI ConnectThread( LPVOID param )
{
	HMODULE moduleReference = (HMODULE)param;
	HMODULE runtime = LoadLibraryEx(L"Windows.UI.Xaml.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
	if (!runtime)
		return FinishConnectThread(moduleReference, NULL, true);

	InitXamlDiagnosticsEx_t init = (InitXamlDiagnosticsEx_t)GetProcAddress(runtime, "InitializeXamlDiagnosticsEx");
	if (!init)
		return FinishConnectThread(moduleReference, runtime, true);

	HMODULE module = GetThisModule();
	wchar_t dllPath[MAX_PATH];
	if (!module || !GetModuleFileName(module, dllPath, _countof(dllPath)))
		return FinishConnectThread(moduleReference, runtime, true);

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
				return FinishConnectThread(moduleReference, runtime, false);
			}
		}
		Sleep(500);
	}

	LogToFile(STARTUP_LOG, L"Win11StartButton: connection failed 0x%08X", last);
	return FinishConnectThread(moduleReference, runtime, true);
}

static void EnsureConnection( void )
{
	if (InterlockedCompareExchange(&g_ConnectStarted, 1, 0) != 0)
		return;

	HMODULE moduleReference = NULL;
	if (!GetModuleHandleEx(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, (LPCTSTR)&ConnectThread, &moduleReference))
	{
		InterlockedExchange(&g_ConnectStarted, 0);
		return;
	}

	HANDLE thread = CreateThread(NULL, 0, ConnectThread, moduleReference, 0, NULL);
	if (thread)
	{
		CloseHandle(thread);
	}
	else
	{
		FreeLibrary(moduleReference);
		InterlockedExchange(&g_ConnectStarted, 0);
	}
}

extern "C" void StartWin11StartButtonTap( BOOL enabled, BOOL allTaskbars )
{
	InterlockedExchange(&g_StartButtonEnabled, enabled ? 1 : 0);
	InterlockedExchange(&g_AllTaskbars, allTaskbars ? 1 : 0);
	InterlockedExchange(&g_StartButtonActive, 1);

	CWin11StartButtonTap *tap = GetTapRef();
	if (tap)
	{
		tap->RequestInputBridgeReprobe();
		tap->RequestApply(false);
		tap->Release();
		return;
	}

	EnsureConnection();
}

extern "C" void UpdateWin11StartButtonTap( BOOL enabled, BOOL allTaskbars )
{
	InterlockedExchange(&g_StartButtonEnabled, enabled ? 1 : 0);
	InterlockedExchange(&g_AllTaskbars, allTaskbars ? 1 : 0);

	CWin11StartButtonTap *tap = GetTapRef();
	if (tap)
	{
		// Re-arm fallback/probing on every taskbar/settings update. Existing
		// route verification may describe XAML handles from the previous layout.
		tap->RequestInputBridgeReprobe();
		tap->RequestApply(false);
		tap->Release();
	}
	else if (InterlockedCompareExchange(&g_StartButtonActive, 0, 0))
	{
		EnsureConnection();
	}
}

extern "C" void StopWin11StartButtonTap( void )
{
	InterlockedExchange(&g_StartButtonActive, 0);

	CWin11StartButtonTap *tap = GetTapRef();
	if (tap)
	{
		HRESULT hr = tap->Deactivate();
		if (FAILED(hr))
			LogToFile(STARTUP_LOG, L"Win11StartButtonTap: deactivate failed 0x%08X", hr);
		tap->Release();
	}
}
