#pragma once

// Windows 11 draws the native Start button in XAML. These helpers hide only
// the native glyph/hit target while Open-Shell's replacement button is active.
// They never move or resize either button.
void StartWin11StartButtonMonitor( void );
void UpdateWin11StartButtonMonitor( void );
void StopWin11StartButtonMonitor( void );

// Returns the current native Windows 11 Start button rectangle in screen
// coordinates. The rectangle comes from the XAML element itself, not from the
// legacy "Start" HWND which is absent on modern Windows 11 taskbars.
bool GetWin11StartButtonRect( HWND taskbar, RECT *rect );
UINT GetWin11StartButtonRectChangedMessage( void );
