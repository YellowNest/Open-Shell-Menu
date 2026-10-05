#pragma once

// Windows 11 draws the native Start button in XAML. These helpers suppress the
// stock XAML icon and expose the real Start slot geometry to Open-Shell's
// replacement button. The geometry comes from UI Automation on a worker MTA,
// never from Explorer's UI thread.
#define WM_OS_STARTBUTTON_RECT_READY (WM_APP + 0x35D)

void StartWin11StartButtonMonitor( void );
void UpdateWin11StartButtonMonitor( void );
void StopWin11StartButtonMonitor( void );

// Returns the most recently verified screen-space rectangle for the native
// Windows 11 Start control. A background refresh is requested automatically if
// no cached rectangle is available.
bool GetWin11StartButtonRect( HWND taskbar, RECT *rect );
void RefreshWin11StartButtonRects( void );
