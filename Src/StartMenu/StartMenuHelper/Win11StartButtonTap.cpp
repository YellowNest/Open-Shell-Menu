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

#undef GetCurrentTime
#include <Windows.UI.Xaml.h>
#include <xamlom.h>
#include <ocidl.h>
#include <atomic>
#include <mutex>
#include <new>
#include <unordered_map>
#include <vector>

static const GUID CLSID_OpenShellStartButtonTap =
{ 0x7d15741f, 0x2f3b, 0x4971, { 0xb8, 0x91, 0x6a, 0x5d, 0x42, 0xd7, 0x1a, 0x34 } };

static const UINT WM_OS_STARTBUTTON_APPLY = WM_APP + 0x35B;
static const UINT WM_OS_STARTBUTTON_DESTROY = WM_APP + 0x35C;
static const UINT WM_OS_STARTBUTTON_RESTORE = WM_APP + 0x35D;
static const wchar_t DISPATCH_WINDOW_CLASS[] = L"OpenShell.Win11StartButtonTap";
static std::mutex g_DispatchClassMutex;

static std::atomic_bool g_StartButtonActive{ false };
static std::atomic_bool g_StartButtonEnabled{ false };
static std::atomic_bool g_AllTaskbars{ false };
static std::atomic_bool g_ConnectStarted{ false };

static HMODULE GetThisModule( void )
{
	HMODULE module = NULL;
	GetModuleHandleEx(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
		(LPCTSTR)&GetThisModule, &module);
	return module;
}

struct StartElement
{
	InstanceHandle parent = 0;
	CString type;
	CString name;
	bool visibilityOverride = false;
	bool hitTestOverride = false;
	bool startControlResolved = false;
	bool isStartControl = false;
	unsigned int discoveryOrder = 0;
};

class CXamlDiagnosticsTap;
static CComPtr<CXamlDiagnosticsTap> g_Tap;
static std::mutex g_TapMutex;

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

// Start-specific tree and property policy, independent of COM TAP lifetime.
class CStartButtonTree
{
public:
	void SetVisual( IVisualTreeService *visual ) { m_Visual = visual; }

	bool OnVisualTreeChange( ParentChildRelation relation, VisualElement element, VisualMutationType mutationType )
	{
		bool interesting = false;

		{
			std::lock_guard lock(m_Mutex);
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
					FindStartAncestorLocked(record.parent) != 0;
			}
		}

		return interesting;
	}

	void ResetElements( void )
	{
		std::lock_guard lock(m_Mutex);
		m_Elements.clear();
		m_PrimaryStart = 0;
		m_NextDiscoveryOrder = 0;
	}

	bool HasOverrides( void )
	{
		std::lock_guard lock(m_Mutex);
		for (const auto &element : m_Elements)
		{
			if (element.second.visibilityOverride || element.second.hitTestOverride)
				return true;
		}
		return false;
	}


	HRESULT ApplyState( bool enabled )
	{
		if (!m_Visual)
			return E_UNEXPECTED;

		HRESULT firstError = S_OK;
		std::vector<std::pair<InstanceHandle, StartElement>> elements;
		{
			std::lock_guard lock(m_Mutex);
			elements.reserve(m_Elements.size());
			for (const auto &element : m_Elements)
				elements.push_back(element);
		}

		// Classification is only needed when applying overrides. During teardown,
		// use the classifications that produced the existing overrides; querying
		// new candidates adds risk and work without helping restoration.
		if (enabled)
		{
			for (size_t i = 0; i < elements.size(); i++)
			{
				StartElement &record = elements[i].second;
				if (!IsStartControlCandidate(record) || record.startControlResolved)
					continue;

				bool isStartControl = false;
				HRESULT hr = ResolveStartControl(elements[i].first, record, &isStartControl);
				if (SUCCEEDED(hr))
				{
					record.startControlResolved = true;
					record.isStartControl = isStartControl;
					SetControlClassification(elements[i].first, isStartControl);
				}
				else if (SUCCEEDED(firstError))
				{
					firstError = hr;
				}
			}
		}

		InstanceHandle primaryStart = 0;
		{
			std::lock_guard lock(m_Mutex);
			primaryStart = m_PrimaryStart;
		}

		const bool allTaskbars = g_AllTaskbars;

		// Both properties share the same set/restore behavior.
		auto applyOverride = [&](InstanceHandle handle, const wchar_t *property, const wchar_t *value,
			bool shouldOverride, bool hasOverride, bool visibility)
		{
			if (shouldOverride == hasOverride)
				return;
			HRESULT hr = shouldOverride ? SetPropertyText(handle, property, value) :
				ClearPropertyByName(handle, property);
			if (SUCCEEDED(hr))
			{
				bool updated = shouldOverride;
				if (visibility)
					SetOverrideFlags(handle, &updated, NULL);
				else
					SetOverrideFlags(handle, NULL, &updated);
			}
			else if (SUCCEEDED(firstError))
				firstError = hr;
		};

		for (size_t i = 0; i < elements.size(); i++)
		{
			InstanceHandle handle = elements[i].first;
			StartElement record = elements[i].second;

			// Restoration depends on what we changed, not on whether a
			// dynamically rebuilt XAML tree still has the same ancestry.
			if (!enabled)
			{
				if (record.hitTestOverride)
					applyOverride(handle, L"IsHitTestVisible", L"False", false, true, false);
				if (record.visibilityOverride)
					applyOverride(handle, L"Visibility", L"Collapsed", false, true, true);
				continue;
			}
			if (record.isStartControl)
			{
				bool target = allTaskbars || !primaryStart || handle == primaryStart;
				applyOverride(handle, L"IsHitTestVisible", L"False",
					enabled && target, record.hitTestOverride, false);
				continue;
			}

			if (!IsStartGlyph(record))
				continue;
			InstanceHandle startAncestor = GetStartAncestor(handle);
			if (!startAncestor)
				continue;
			bool target = allTaskbars || !primaryStart || startAncestor == primaryStart;
			applyOverride(handle, L"Visibility", L"Collapsed",
				enabled && target, record.visibilityOverride, true);
		}

		return firstError;
	}


private:
	InstanceHandle FindStartAncestorLocked( InstanceHandle parent ) const
	{
		for (int depth = 0; depth < 24 && parent; depth++)
		{
			auto it = m_Elements.find(parent);
			if (it == m_Elements.end())
				break;
			if (it->second.isStartControl)
				return parent;
			parent = it->second.parent;
		}
		return 0;
	}

	InstanceHandle GetStartAncestor( InstanceHandle handle )
	{
		std::lock_guard lock(m_Mutex);
		auto it = m_Elements.find(handle);
		return it != m_Elements.end() ? FindStartAncestorLocked(it->second.parent) : 0;
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
		std::lock_guard lock(m_Mutex);
		auto it = m_Elements.find(handle);
		if (it != m_Elements.end())
		{
			it->second.startControlResolved = true;
			it->second.isStartControl = isStartControl;
			m_PrimaryStart = FindPrimaryStartLocked();
		}
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
		std::lock_guard lock(m_Mutex);
		auto it = m_Elements.find(handle);
		if (it != m_Elements.end())
		{
			if (visibility)
				it->second.visibilityOverride = *visibility;
			if (hitTest)
				it->second.hitTestOverride = *hitTest;
		}
	}

	CComPtr<IVisualTreeService> m_Visual;
	std::mutex m_Mutex;
	InstanceHandle m_PrimaryStart = 0;
	unsigned int m_NextDiscoveryOrder = 0;
	// Keep ancestry from replay for Start-button glyph classification.
	std::unordered_map<InstanceHandle, StartElement> m_Elements;
};

class CXamlDiagnosticsTap: public IObjectWithSite, public IVisualTreeServiceCallback2
{
public:
	CXamlDiagnosticsTap( void )
	{
		_AtlModule.Lock();
		// Construct the dispatch window once; deactivation removes it.
		m_DispatchStatus = CreateDispatchWindow();
	}

	~CXamlDiagnosticsTap( void )
	{
		// WM_NCDESTROY releases the dispatch window's own COM reference.
		ATLASSERT(m_Dispatch == NULL);
		_AtlModule.Unlock();
	}

	HRESULT DispatchStatus( void ) const { return m_DispatchStatus; }

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
		return ++m_Refs;
	}

	STDMETHODIMP_(ULONG) Release( void )
	{
		ULONG refs = --m_Refs;
		if (!refs)
			delete this;
		return refs;
	}

	STDMETHODIMP SetSite( IUnknown *site )
	{
		// NULL clears the site. During Unadvise, the diagnostics framework may
		// enter SetSite again; the in-progress shutdown already owns cleanup.
		if (!site)
			return m_Deactivating ? S_OK : Deactivate();

		std::lock_guard lifecycleLock(m_LifecycleMutex);
		if (m_SiteAssigned)
			return HRESULT_FROM_WIN32(ERROR_ALREADY_INITIALIZED);

		CComPtr<IVisualTreeService> visual;
		HRESULT hr = site->QueryInterface(__uuidof(IVisualTreeService), (void**)&visual);
		if (FAILED(hr))
			return hr;
		if (!visual)
			return E_NOINTERFACE;

		bool active;
		{
			std::unique_lock lock(g_TapMutex);
			active = g_StartButtonActive;
			if (active && g_Tap)
				return HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS);

			m_SiteAssigned = true;
			if (active)
			{
				{
					std::lock_guard siteLock(m_SiteMutex);
					m_Site = site;
				}
				m_Visual = visual;
				m_StartTree.SetVisual(visual);
				g_Tap = this;
			}
			else
				g_ConnectStarted = false;
		}
		if (!active)
		{
			// An unused site must not pin a dispatch window forever.
			HRESULT closeHr = DestroyDispatchWindow();
			if (FAILED(closeHr))
				LogToFile(STARTUP_LOG, L"Win11StartButtonTap: idle window close failed 0x%08X", closeHr);
			return S_OK;
		}

		hr = ActivateLocked();
		if (FAILED(hr))
		{
			// Retain any callback that a failed Unadvise could not unregister.
			if (!m_Advised)
			{
				// No callback remains registered; close the unused window.
				HRESULT closeHr = DestroyDispatchWindow();
				if (FAILED(closeHr))
					LogToFile(STARTUP_LOG, L"Win11StartButtonTap: activation close failed 0x%08X", closeHr);
				CComPtr<CXamlDiagnosticsTap> released;
				{
					std::unique_lock lock(g_TapMutex);
					if (g_Tap.p == this)
						released.Attach(g_Tap.Detach());
					g_ConnectStarted = false;
				}
			}
			LogToFile(STARTUP_LOG, L"Win11StartButtonTap: activation failed 0x%08X", hr);
		}
		return hr;
	}

	STDMETHODIMP GetSite( REFIID riid, void **ppv )
	{
		if (!ppv)
			return E_POINTER;
		*ppv = NULL;
		CComPtr<IUnknown> site;
		{
			// Hold an independent reference before another thread clears m_Site.
			std::lock_guard siteLock(m_SiteMutex);
			site = m_Site;
		}
		return site ? site->QueryInterface(riid, ppv) : E_FAIL;
	}

	STDMETHODIMP OnVisualTreeChange( ParentChildRelation relation, VisualElement element, VisualMutationType mutationType )
	{
		// std::vector, CString and unordered_map may allocate. Never let
		// a C++ exception unwind through a COM callback in Explorer.
		try
		{
			if (m_StartTree.OnVisualTreeChange(relation, element, mutationType) && m_Advised && m_AllowEnable)
				RequestApply(g_StartButtonActive && g_StartButtonEnabled);
		}
		catch (...)
		{
			m_CallbackFailed = true;
			m_AllowEnable = false;
		}
		return S_OK;
	}

	STDMETHODIMP OnElementStateChanged( InstanceHandle, VisualElementState, LPCWSTR )
	{
		return S_OK;
	}

	HRESULT Activate( void )
	{
		std::lock_guard lock(m_LifecycleMutex);
		return ActivateLocked();
	}

	HRESULT Deactivate( void )
	{
		CComPtr<CXamlDiagnosticsTap> releasedTap;
		HRESULT hr;
		{
			std::lock_guard lock(m_LifecycleMutex);
			hr = DeactivateLocked();
			if (SUCCEEDED(hr))
			{
				std::unique_lock tapLock(g_TapMutex);
				if (g_Tap.p == this)
				{
					releasedTap.Attach(g_Tap.Detach());
					g_ConnectStarted = false;
				}
			}
		}
		return hr;
	}

private:
	// Only teardown needs synchronous delivery; ordinary updates just post work.
	HRESULT RequestApply( bool enabled )
	{
		HWND dispatch = m_Dispatch;
		if (!dispatch)
			return HRESULT_FROM_WIN32(ERROR_INVALID_WINDOW_HANDLE);
		return PostMessage(dispatch, WM_OS_STARTBUTTON_APPLY, enabled ? 1 : 0, 0)
			? S_OK : HRESULT_FROM_WIN32(GetLastError());
	}

	HRESULT RestoreOnDispatch( void )
	{
		HWND dispatch = m_Dispatch;
		if (!dispatch)
			return m_StartTree.HasOverrides() ? HRESULT_FROM_WIN32(ERROR_INVALID_WINDOW_HANDLE) : S_OK;

		DWORD_PTR result = 0;
		if (!SendMessageTimeout(dispatch, WM_OS_STARTBUTTON_RESTORE, 0, 0,
			SMTO_ABORTIFHUNG | SMTO_BLOCK, 2000, &result))
		{
			DWORD error = GetLastError();
			return HRESULT_FROM_WIN32(error ? error : ERROR_TIMEOUT);
		}
		return static_cast<HRESULT>(result);
	}

	HRESULT ActivateLocked( void )
	{
		if (m_CallbackFailed)
			return E_FAIL;
		if (!m_Visual)
			return E_UNEXPECTED;
		if (FAILED(m_DispatchStatus))
			return m_DispatchStatus;

		if (!m_Advised)
		{
			m_StartTree.ResetElements();

			// Advise replays the existing tree synchronously. Until it returns,
			// OnVisualTreeChange records elements without scheduling partial work.
			HRESULT hr = m_Visual->AdviseVisualTreeChange(
				static_cast<IVisualTreeServiceCallback*>(this));
			if (FAILED(hr))
			{
				m_StartTree.ResetElements();
				return hr;
			}
			m_Advised = true;
			// An exception during the synchronous tree replay leaves only a
			// partial tree. Refuse to apply overrides from that state.
			if (m_CallbackFailed)
				return E_FAIL;
		}

		m_AllowEnable = true;
		return RequestApply(g_StartButtonActive && g_StartButtonEnabled);
	}

	HRESULT DeactivateLocked( void )
	{
		if (!m_Visual)
			return DestroyDispatchWindow();

		// First reject queued enable work. The dispatch thread drains queued
		// messages and restores native properties before we unsubscribe.
		m_Deactivating = true;
		m_AllowEnable = false;
		HRESULT hr = RestoreOnDispatch();
		if (FAILED(hr))
		{
			LogToFile(STARTUP_LOG,
				L"Win11StartButtonTap: synchronous restore failed 0x%08X", hr);
			m_Deactivating = false;
			return hr;
		}

		// Keep both the callback and dispatch window alive until Unadvise
		// completes; then close the idle window.
		if (m_Visual && m_Advised)
		{
			hr = m_Visual->UnadviseVisualTreeChange(
				static_cast<IVisualTreeServiceCallback*>(this));
			if (FAILED(hr))
			{
				LogToFile(STARTUP_LOG,
					L"Win11StartButtonTap: visual tree unadvise failed 0x%08X", hr);
				m_Deactivating = false;
				return hr;
			}
			m_Advised = false;
		}

		// Dispose of the window while the TAP is still strongly referenced.
		// Otherwise a cross-thread destroy message could enter a dying object.
		hr = DestroyDispatchWindow();
		if (FAILED(hr))
		{
			m_Deactivating = false;
			return hr;
		}

		m_StartTree.ResetElements();
		m_StartTree.SetVisual(NULL);
		m_Visual.Release();
		CComPtr<IUnknown> releasedSite;
		{
			std::lock_guard siteLock(m_SiteMutex);
			releasedSite.Attach(m_Site.Detach());
		}
		// Release outside the site mutex: COM may call GetSite again.
		releasedSite.Release();
		m_Deactivating = false;
		return S_OK;
	}

	HRESULT CreateDispatchWindow( void )
	{
		HWND dispatch = m_Dispatch;
		if (dispatch)
		{
			if (IsWindow(dispatch))
				return S_OK;
			m_Dispatch = NULL;
		}

		WNDCLASS wc = {};
		HMODULE module = GetThisModule();
		if (!module)
			return E_FAIL;

		// Window classes registered by DLLs survive FreeLibrary. Register
		// and create together, and unregister after the last window closes.
		std::lock_guard classLock(g_DispatchClassMutex);
		wc.lpfnWndProc = DispatchProc;
		wc.hInstance = module;
		wc.lpszClassName = DISPATCH_WINDOW_CLASS;
		if (!RegisterClass(&wc))
		{
			DWORD error = GetLastError();
			if (error != ERROR_CLASS_ALREADY_EXISTS)
				return HRESULT_FROM_WIN32(error);
		}

		dispatch = CreateWindowEx(0, DISPATCH_WINDOW_CLASS, L"", 0, 0, 0, 0, 0,
			HWND_MESSAGE, NULL, module, this);
		if (!dispatch)
		{
			DWORD error = GetLastError();
			UnregisterClass(DISPATCH_WINDOW_CLASS, module);
			return HRESULT_FROM_WIN32(error);
		}

		m_Dispatch = dispatch;
		return S_OK;
	}

	HRESULT DestroyDispatchWindow( void )
	{
		HWND dispatch = m_Dispatch;
		if (!dispatch)
			return S_OK;
		if (!IsWindow(dispatch))
		{
			m_Dispatch = NULL;
			std::lock_guard classLock(g_DispatchClassMutex);
			UnregisterClass(DISPATCH_WINDOW_CLASS, GetThisModule());
			return S_OK;
		}

		if (GetWindowThreadProcessId(dispatch, NULL) == GetCurrentThreadId())
		{
			DrainApplyMessages(dispatch);
			SetWindowLongPtr(dispatch, GWLP_USERDATA, 0);
			if (!DestroyWindow(dispatch))
			{
				DWORD error = GetLastError();
				if (IsWindow(dispatch))
					SetWindowLongPtr(dispatch, GWLP_USERDATA, (LONG_PTR)this);
				return HRESULT_FROM_WIN32(error);
			}
			m_Dispatch = NULL;
			std::lock_guard classLock(g_DispatchClassMutex);
			UnregisterClass(DISPATCH_WINDOW_CLASS, GetThisModule());
			return S_OK;
		}

		DWORD_PTR result = 0;
		SetLastError(ERROR_SUCCESS);
		LRESULT sent = SendMessageTimeout(dispatch, WM_OS_STARTBUTTON_DESTROY, 0, 0,
			SMTO_ABORTIFHUNG | SMTO_BLOCK, 2000, &result);
		if (!sent && IsWindow(dispatch))
		{
			DWORD error = GetLastError();
			if (!error)
				error = ERROR_TIMEOUT;
			return HRESULT_FROM_WIN32(error);
		}

		if (IsWindow(dispatch))
			return E_FAIL;

		m_Dispatch = NULL;
		return S_OK;
	}

	static void DrainApplyMessages( HWND hwnd )
	{
		MSG pending;
		while (PeekMessage(&pending, hwnd, WM_OS_STARTBUTTON_APPLY, WM_OS_STARTBUTTON_APPLY, PM_REMOVE))
		{
		}
	}

	LRESULT ApplyTreeSafely( bool enabled )
	{
		try
		{
			return static_cast<LRESULT>(m_StartTree.ApplyState(enabled));
		}
		catch (...)
		{
			m_AllowEnable = false;
			return static_cast<LRESULT>(E_FAIL);
		}
	}

	static LRESULT CALLBACK DispatchProc( HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam )
	{
		CXamlDiagnosticsTap *tap = (CXamlDiagnosticsTap*)GetWindowLongPtr(hwnd, GWLP_USERDATA);
		if (msg == WM_NCCREATE)
		{
			CREATESTRUCT *create = (CREATESTRUCT*)lParam;
			tap = (CXamlDiagnosticsTap*)create->lpCreateParams;
			// The HWND retains a strong reference until WM_NCDESTROY.
			tap->AddRef();
			SetWindowLongPtr(hwnd, GWLP_USERDATA, (LONG_PTR)tap);
		}
		if (msg == WM_NCDESTROY && tap)
		{
			// This is the final message: clear the raw pointer before
			// releasing the HWND's own COM reference.
			SetWindowLongPtr(hwnd, GWLP_USERDATA, 0);
			if (tap->m_Dispatch == hwnd)
				tap->m_Dispatch = NULL;
			LRESULT result = DefWindowProc(hwnd, msg, wParam, lParam);
			tap->Release();
			return result;
		}
		if (msg == WM_OS_STARTBUTTON_DESTROY && tap)
			return static_cast<LRESULT>(tap->DestroyDispatchWindow());
		if (msg == WM_OS_STARTBUTTON_RESTORE && tap)
		{
			DrainApplyMessages(hwnd);
			return tap->ApplyTreeSafely(false);
		}
		if (msg == WM_OS_STARTBUTTON_APPLY && tap)
		{
			// The synchronous restore is the only tree mutation permitted
			// after shutdown begins. Late posts must not access m_Visual.
			if (!tap->m_AllowEnable)
				return S_OK;
			bool enabled = wParam != 0 && g_StartButtonActive && g_StartButtonEnabled;
			return tap->ApplyTreeSafely(enabled);
		}
		return DefWindowProc(hwnd, msg, wParam, lParam);
	}

	friend class CStartButtonTapFactory;
	std::atomic<ULONG> m_Refs{ 1 };
	HRESULT m_DispatchStatus = E_UNEXPECTED;
	std::atomic_bool m_Advised{ false };
	std::atomic_bool m_AllowEnable{ false };
	std::atomic_bool m_CallbackFailed{ false };
	std::atomic_bool m_Deactivating{ false };
	std::atomic<HWND> m_Dispatch{ NULL };
	std::mutex m_LifecycleMutex;
	std::mutex m_SiteMutex;
	bool m_SiteAssigned = false;
	CComPtr<IUnknown> m_Site;
	CComPtr<IVisualTreeService> m_Visual;
	CStartButtonTree m_StartTree;
};

static CComPtr<CXamlDiagnosticsTap> GetTapRef( void )
{
	std::lock_guard lock(g_TapMutex);
	return g_Tap;
}

class CStartButtonTapFactory: public IClassFactory
{
public:
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

	STDMETHODIMP_(ULONG) AddRef( void ) { return ++m_Refs; }
	STDMETHODIMP_(ULONG) Release( void ) { return --m_Refs; }

	STDMETHODIMP CreateInstance( IUnknown *outer, REFIID riid, void **ppv )
	{
		if (!ppv)
			return E_POINTER;
		*ppv = NULL;
		if (outer)
			return CLASS_E_NOAGGREGATION;

		CComPtr<CXamlDiagnosticsTap> tap;
		tap.Attach(new (std::nothrow) CXamlDiagnosticsTap());
		if (!tap)
			return E_OUTOFMEMORY;
		HRESULT hr = tap->DispatchStatus();
		if (SUCCEEDED(hr))
			hr = tap->QueryInterface(riid, ppv);
		if (FAILED(hr))
			tap->DestroyDispatchWindow();
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
	std::atomic<ULONG> m_Refs{ 1 };
};

static CStartButtonTapFactory g_Factory;

HRESULT GetWin11StartButtonTapClassObject( REFCLSID clsid, REFIID riid, LPVOID *ppv )
{
	if (!IsEqualGUID(clsid, CLSID_OpenShellStartButtonTap))
		return CLASS_E_CLASSNOTAVAILABLE;
	return g_Factory.QueryInterface(riid, ppv);
}

typedef HRESULT (WINAPI *InitXamlDiagnosticsEx_t)( LPCWSTR, DWORD, LPCWSTR, LPCWSTR, CLSID, LPCWSTR );

static DWORD WINAPI ConnectThread( LPVOID param )
{
	HMODULE moduleReference = (HMODULE)param;
	HRESULT last = E_FAIL;
	HMODULE runtime = LoadLibraryEx(L"Windows.UI.Xaml.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
	if (runtime)
	{
		InitXamlDiagnosticsEx_t init =
			(InitXamlDiagnosticsEx_t)GetProcAddress(runtime, "InitializeXamlDiagnosticsEx");
		if (init)
		{
			HMODULE module = GetThisModule();
			wchar_t dllPath[MAX_PATH];
			DWORD length = module ? GetModuleFileName(module, dllPath, _countof(dllPath)) : 0;
			if (length && length < _countof(dllPath))
			{
				const wchar_t *endpoints[] = { L"VisualDiagConnection1", L"VisualDiagConnection2" };
				for (int retry = 0; retry < 8 && g_StartButtonActive && FAILED(last); retry++)
				{
					for (int i = 0; i < _countof(endpoints); i++)
					{
						last = init(endpoints[i], GetCurrentProcessId(), NULL,
							dllPath, CLSID_OpenShellStartButtonTap, NULL);
						if (SUCCEEDED(last))
						{
							LogToFile(STARTUP_LOG, L"Win11StartButton: connected using %s", endpoints[i]);
							break;
						}
					}
					if (FAILED(last) && retry < 7 && g_StartButtonActive)
						Sleep(500);
				}
			}
		}
		FreeLibrary(runtime);
	}

	if (FAILED(last))
	{
		g_ConnectStarted = false;
		LogToFile(STARTUP_LOG, L"Win11StartButton: connection failed 0x%08X", last);
	}

	// Release the worker's self-reference without returning into an unloaded DLL.
	FreeLibraryAndExitThread(moduleReference, 0);
	return 0;
}

static void EnsureConnection( void )
{
	// Connection probing retries endpoints and may sleep, so keep it off the
	// Explorer taskbar thread. ConnectThread is the only worker we need here.
	bool expected = false;
	if (!g_ConnectStarted.compare_exchange_strong(expected, true))
		return;

	HMODULE moduleReference = NULL;
	if (GetModuleHandleEx(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, (LPCTSTR)&ConnectThread, &moduleReference))
	{
		HANDLE thread = CreateThread(NULL, 0, ConnectThread, moduleReference, 0, NULL);
		if (thread)
		{
			CloseHandle(thread);
			return;
		}
		FreeLibrary(moduleReference);
	}

	// A worker was not started, so connection can be attempted again later.
	g_ConnectStarted = false;
}

static void ActivateCurrentTap( bool enabled )
{
	auto tap = GetTapRef();
	if (!tap)
	{
		// A disabled replacement needs no injected XAML diagnostics session.
		if (enabled)
			EnsureConnection();
		return;
	}
	HRESULT hr = tap->Activate();
	if (FAILED(hr))
		LogToFile(STARTUP_LOG, L"Win11StartButtonTap: activation failed 0x%08X", hr);
}

extern "C" void StartWin11StartButtonTap( BOOL enabled, BOOL allTaskbars )
{
	g_StartButtonEnabled = enabled != FALSE;
	g_AllTaskbars = allTaskbars != FALSE;
	g_StartButtonActive = true;
	ActivateCurrentTap(enabled != FALSE);
}

extern "C" void UpdateWin11StartButtonTap( BOOL enabled, BOOL allTaskbars )
{
	g_StartButtonEnabled = enabled != FALSE;
	g_AllTaskbars = allTaskbars != FALSE;
	if (g_StartButtonActive)
		ActivateCurrentTap(enabled != FALSE);
}

extern "C" void StopWin11StartButtonTap( void )
{
	CComPtr<CXamlDiagnosticsTap> tap;
	{
		// Serialize stop with SetSite publishing the global TAP pointer.
		std::unique_lock lock(g_TapMutex);
		g_StartButtonActive = false;
		tap = g_Tap;
	}
	if (tap)
	{
		HRESULT hr = tap->Deactivate();
		if (FAILED(hr))
			LogToFile(STARTUP_LOG, L"Win11StartButtonTap: deactivate failed 0x%08X", hr);
	}
}
