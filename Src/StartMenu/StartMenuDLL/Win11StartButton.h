#pragma once

// Windows 11 draws the native Start button in XAML. These helpers hide only
// the native glyph/hit target while Open-Shell's replacement button is active.
// They never move or resize either button.
void StartWin11StartButtonMonitor( void );
void UpdateWin11StartButtonMonitor( void );
void StopWin11StartButtonMonitor( void );

// Track the native Windows 11 Start control's real screen position while the
// centered taskbar animates/reflows, so the replacement button follows it.
void RegisterWin11StartButtonTracking( HWND taskbar );
bool GetTrackedWin11StartButtonRect( HWND taskbar, RECT *rect );
UINT GetWin11StartButtonTrackingMessage( void );
