// Berkeley Open Infrastructure for Network Computing
// http://boinc.berkeley.edu
// Copyright (C) 2005 University of California
//
// This is free software; you can redistribute it and/or
// modify it under the terms of the GNU Lesser General Public
// License as published by the Free Software Foundation;
// either version 2.1 of the License, or (at your option) any later version.
//
// This software is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
// See the GNU Lesser General Public License for more details.
//
// To view the GNU Lesser General Public License visit
// http://www.gnu.org/copyleft/lesser.html
// or write to the Free Software Foundation, Inc.,
// 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA

#include "boinc_win.h"

#include <windowsx.h>
#include <mmsystem.h>
#include <regstr.h>
#include <shlobj.h>
#define COMPILE_MULTIMON_STUBS
#include <multimon.h>
#include <strsafe.h>

#include "diagnostics.h"
#include "exception.h"
#include "boinc_ss.h"
#include "win_screensaver.h"
#include "win_util.h"


//
// Define the stuff needed to actually do a set foreground window on
//   Windows 2000 or later machines.
//
#ifndef BSF_ALLOWSFW
#define BSF_ALLOWSFW            0x00000080
#endif

const UINT WM_BOINCSFW = RegisterWindowMessage(TEXT("BOINCSetForegroundWindow"));


static CScreensaver* gs_pScreensaver = NULL;


INT WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow)
{
    HRESULT      hr;
    int          retval;
    WSADATA      wsdata;
    CScreensaver BOINCSS;


#ifdef _DEBUG
    // Initialize Diagnostics when compiled for debug
    retval = boinc_init_diagnostics (
        BOINC_DIAG_DUMPCALLSTACKENABLED | 
        BOINC_DIAG_HEAPCHECKENABLED |
        BOINC_DIAG_MEMORYLEAKCHECKENABLED |
        BOINC_DIAG_ARCHIVESTDERR |
        BOINC_DIAG_REDIRECTSTDERROVERWRITE |
        BOINC_DIAG_TRACETOSTDERR
    );
    if (retval) 
    {
        BOINCTRACE("WinMain - BOINC Screensaver Diagnostic Error '%d'", retval);
        MessageBox(NULL, NULL, "BOINC Screensaver Diagnostic Error", MB_OK);
    }
#endif


    retval = WSAStartup( MAKEWORD( 1, 1 ), &wsdata);
    if (retval) 
    {
        BOINCTRACE("WinMain - Winsock Initialization Failure '%d'", retval);
        return retval;
    }


    if( FAILED( hr = BOINCSS.Create( hInstance ) ) )
    {
        BOINCSS.DisplayErrorMsg( hr );
        WSACleanup();
        return 0;
    }


    retval = BOINCSS.Run();


    WSACleanup();


    return retval;
}


//-----------------------------------------------------------------------------
// Name: CScreensaver()
// Desc: Constructor
//-----------------------------------------------------------------------------
CScreensaver::CScreensaver()
{
    gs_pScreensaver = this;

    m_bCheckingSaverPassword = FALSE;
    m_bIs9x = FALSE;
    m_dwSaverMouseMoveCount = 0;
    m_hWnd = NULL;
    m_hWndParent = NULL;
    m_hPasswordDLL = NULL;
    m_VerifySaverPassword = NULL;
    
    m_bAllScreensSame = FALSE;
    m_bWindowed = FALSE;
    m_bWaitForInputIdle = FALSE;

    m_bErrorMode = FALSE;
    m_hrError = S_OK;
    m_szError[0] = _T('\0');

    LoadString( NULL, IDS_DESCRIPTION, m_strWindowTitle, 200 );

	m_bPaintingInitialized = FALSE;
	m_bBOINCCoreNotified = FALSE;
    m_bResetCoreState = TRUE;
    m_dwBOINCTimerCounter = 0;
    m_dwBlankTime = 0;

    m_bBOINCConfigChecked = FALSE;
    m_bBOINCStartupConfigured = FALSE;

	ZeroMemory( m_Monitors, sizeof(m_Monitors) );
    m_dwNumMonitors = 0;
}


//-----------------------------------------------------------------------------
// Name: Create()
// Desc: Have the client program call this function before calling Run().
//-----------------------------------------------------------------------------
HRESULT CScreensaver::Create( HINSTANCE hInstance )
{
    HRESULT hr;
    BOOL    bReturnValue;

    m_hInstance = hInstance;

    // Parse the command line and do the appropriate thing
    m_SaverMode = ParseCommandLine( GetCommandLine() );

    // Figure out if we're on Win9x
    OSVERSIONINFO osvi; 
    osvi.dwOSVersionInfoSize = sizeof(osvi);
    GetVersionEx( &osvi );
    m_bIs9x = (osvi.dwPlatformId == VER_PLATFORM_WIN32_WINDOWS);

    // Enumerate Monitors
    EnumMonitors();


    // Retrieve the blank screen timeout
	// make sure you check return value of registry queries
	// in case the item in question doesn't happen to exist.
	bReturnValue = UtilGetRegKey( REG_BLANK_TIME, m_dwBlankTime );
	if ( bReturnValue != 0 ) m_dwBlankTime = 5;

    // Calculate the estimated blank time by adding the current time
    //   and and the user specified time which is in minutes
    m_dwBlankTime = time(0) + (m_dwBlankTime * 60);

    // Save the value back to the registry in case this is the first
    // execution and so we need the default value later.
	UtilSetRegKey( REG_BLANK_TIME, m_dwBlankTime );


    // Create the screen saver window(s)
    if( m_SaverMode == sm_preview || 
        m_SaverMode == sm_test    || 
        m_SaverMode == sm_full )
    {
        if( FAILED( hr = CreateSaverWindow() ) )
        {
            m_bErrorMode = TRUE;
            m_hrError = hr;
        }
    }

    if( m_SaverMode == sm_preview )
    {
        // In preview mode, "pause" (enter a limited message loop) briefly 
        // before proceeding, so the display control panel knows to update itself.
        m_bWaitForInputIdle = TRUE;

        // Post a message to mark the end of the initial group of window messages
        PostMessage( m_hWnd, WM_USER, 0, 0 );

        MSG msg;
        while( m_bWaitForInputIdle )
        {
            // If GetMessage returns FALSE, it's quitting time.
            if( !GetMessage( &msg, m_hWnd, 0, 0 ) )
            {
                // Post the quit message to handle it later
                PostQuitMessage(0);
                break;
            }

            TranslateMessage( &msg );
            DispatchMessage( &msg );
        }
    }


    return S_OK;
}




//-----------------------------------------------------------------------------
// Name: Run()
// Desc: Starts main execution of the screen saver.
//-----------------------------------------------------------------------------
INT CScreensaver::Run()
{
    HRESULT hr;

    // Parse the command line and do the appropriate thing
    switch ( m_SaverMode )
    {
        case sm_config:
        {
            if( m_bErrorMode )
            {
                DisplayErrorMsg( m_hrError );
            }
            else
            {
                DoConfig();
            }
            break;
        }
        
        case sm_preview:
        case sm_test:
        case sm_full:
        {
            if( FAILED( hr = DoSaver() ) )
                DisplayErrorMsg( hr );
            break;
        }
        
        case sm_passwordchange:
        {
            ChangePassword();
            break;
        }
    }

    return 0;
}




//-----------------------------------------------------------------------------
// Name: StartupBOINC()
// Desc: Notifies BOINC that it has to start the screensaver in full screen mode.
//-----------------------------------------------------------------------------
VOID CScreensaver::StartupBOINC()
{
    int iStatus = 0;

	if( m_SaverMode != sm_preview )
	{
        if( (NULL != m_Monitors[0].hWnd) && (m_bBOINCCoreNotified == FALSE) )
		{
            TCHAR szCurrentWindowStation[MAX_PATH];
            TCHAR szCurrentDesktop[MAX_PATH];
            BOOL  bReturnValue;
            int   iReturnValue;

            memset(szCurrentWindowStation, 0, sizeof(szCurrentWindowStation)/sizeof(TCHAR));
            memset(szCurrentDesktop, 0, sizeof(szCurrentDesktop)/sizeof(TCHAR));

            if (!m_bIs9x)
            {
                // Retrieve the current window station and desktop names
                bReturnValue = GetUserObjectInformation( 
                    GetProcessWindowStation(), 
                    UOI_NAME, 
                    szCurrentWindowStation,
                    (sizeof(szCurrentWindowStation) / sizeof(TCHAR)),
                    NULL
                );
                if (!bReturnValue)
                {
                    BOINCTRACE(_T("Failed to retrieve the current window station.\n"));
                }

                bReturnValue = GetUserObjectInformation( 
                    GetThreadDesktop(GetCurrentThreadId()), 
                    UOI_NAME, 
                    szCurrentDesktop,
                    (sizeof(szCurrentDesktop) / sizeof(TCHAR)),
                    NULL
                );
                if (!bReturnValue)
                {
                    BOINCTRACE(_T("Failed to retrieve the current desktop.\n"));
                }
            }

			// Tell the boinc client to start the screen saver
            BOINCTRACE(
                _T("CScreensaver::StartupBOINC - Calling set_screensaver_mode - WindowStation = '%s', Desktop = '%s', BlankTime = '%d'.\n"),
                szCurrentWindowStation, szCurrentDesktop, m_dwBlankTime
            );
            iReturnValue = rpc.set_screensaver_mode(true, szCurrentWindowStation, szCurrentDesktop, m_dwBlankTime);
            BOINCTRACE(_T("CScreensaver::StartupBOINC - set_screensaver_mode iReturnValue = '%d'\n"), iReturnValue);

			// We have now notified the boinc client
			if ( 0 == iReturnValue )
                m_bBOINCCoreNotified = TRUE;
            else
            {
       			m_bErrorMode = TRUE;
    			m_hrError = SCRAPPERR_BOINCNOTDETECTED;
            }
		}
	}
}




//-----------------------------------------------------------------------------
// Name: RenderBOINC()
// Desc: Notifies BOINC that it has to start the screensaver in full screen mode.
//-----------------------------------------------------------------------------
VOID CScreensaver::ShutdownBOINC()
{
	if( m_bBOINCCoreNotified )
	{
		// Tell the boinc client to stop the screen saver
        rpc.set_screensaver_mode(false, NULL, NULL, 0.0);

        // We have now notified the boinc client
		m_bBOINCCoreNotified = FALSE;
	}
}




//-----------------------------------------------------------------------------
// Name: ParseCommandLine()
// Desc: Interpret command-line parameters passed to this app.
//-----------------------------------------------------------------------------
SaverMode CScreensaver::ParseCommandLine( TCHAR* pstrCommandLine )
{
    m_hWndParent = NULL;

	BOINCTRACE("ParseCommandLine: '%s'\n", pstrCommandLine);

    // Skip the first part of the command line, which is the full path 
    // to the exe.  If it contains spaces, it will be contained in quotes.
    if (*pstrCommandLine == _T('\"'))
    {
        pstrCommandLine++;
        while (*pstrCommandLine != _T('\0') && *pstrCommandLine != _T('\"'))
            pstrCommandLine++;
        if( *pstrCommandLine == _T('\"') )
            pstrCommandLine++;
    }
    else
    {
        while (*pstrCommandLine != _T('\0') && *pstrCommandLine != _T(' '))
            pstrCommandLine++;
        if( *pstrCommandLine == _T(' ') )
            pstrCommandLine++;
    }

    // Skip along to the first option delimiter "/" or "-"
    while ( *pstrCommandLine != _T('\0') && *pstrCommandLine != _T('/') && *pstrCommandLine != _T('-') )
        pstrCommandLine++;

    // If there wasn't one, then must be config mode
    if ( *pstrCommandLine == _T('\0') )
        return sm_config;

    // Otherwise see what the option was
    switch ( *(++pstrCommandLine) )
    {
        case 'c':
        case 'C':
            pstrCommandLine++;
            while ( *pstrCommandLine && !isdigit(*pstrCommandLine) )
                pstrCommandLine++;
            if ( isdigit(*pstrCommandLine) )
            {
#ifdef _WIN64
                m_hWndParent = HWND(_atoi64(pstrCommandLine));
#else
                m_hWndParent = HWND(_ttol(pstrCommandLine));
#endif
            }
            else
            {
                m_hWndParent = NULL;
            }
            return sm_config;

        case 't':
        case 'T':
            return sm_test;

        case 'p':
        case 'P':
            // Preview-mode, so option is followed by the parent HWND in decimal
            pstrCommandLine++;
            while ( *pstrCommandLine && !isdigit(*pstrCommandLine) )
                pstrCommandLine++;
            if ( isdigit(*pstrCommandLine) )
            {
#ifdef _WIN64
                m_hWndParent = HWND(_atoi64(pstrCommandLine));
#else
                m_hWndParent = HWND(_ttol(pstrCommandLine));
#endif
            }
            return sm_preview;

        case 'a':
        case 'A':
            // Password change mode, so option is followed by parent HWND in decimal
            pstrCommandLine++;
            while ( *pstrCommandLine && !isdigit(*pstrCommandLine) )
                pstrCommandLine++;
            if ( isdigit(*pstrCommandLine) )
            {
#ifdef _WIN64
                m_hWndParent = HWND(_atoi64(pstrCommandLine));
#else
                m_hWndParent = HWND(_ttol(pstrCommandLine));
#endif
            }
            return sm_passwordchange;

        default:
            // All other options => run the screensaver (typically this is "/s")
            return sm_full;
    }
}




//-----------------------------------------------------------------------------
// Name: EnumMonitors()
// Desc: Determine HMONITOR, desktop rect, and other info for each monitor.  
//       Note that EnumDisplayDevices enumerates monitors in the order 
//       indicated on the Settings page of the Display control panel, which 
//       is the order we want to list monitors in, as opposed to the order 
//       used by D3D's GetAdapterInfo.
//-----------------------------------------------------------------------------
VOID CScreensaver::EnumMonitors( VOID )
{
    DWORD iDevice = 0;
    DISPLAY_DEVICE_FULL dispdev;
    DISPLAY_DEVICE_FULL dispdev2;
    DEVMODE devmode;
    dispdev.cb = sizeof(dispdev);
    dispdev2.cb = sizeof(dispdev2);
    devmode.dmSize = sizeof(devmode);
    devmode.dmDriverExtra = 0;
    INTERNALMONITORINFO* pMonitorInfoNew;
    while( EnumDisplayDevices(NULL, iDevice, (DISPLAY_DEVICE*)&dispdev, 0) )
    {
        // Ignore NetMeeting's mirrored displays
        if( (dispdev.StateFlags & DISPLAY_DEVICE_MIRRORING_DRIVER) == 0 )
        {
            // To get monitor info for a display device, call EnumDisplayDevices
            // a second time, passing dispdev.DeviceName (from the first call) as
            // the first parameter.
            EnumDisplayDevices(dispdev.DeviceName, 0, (DISPLAY_DEVICE*)&dispdev2, 0);

            pMonitorInfoNew = &m_Monitors[m_dwNumMonitors];
            ZeroMemory( pMonitorInfoNew, sizeof(INTERNALMONITORINFO) );
            StringCchCopy( pMonitorInfoNew->strDeviceName, 128, dispdev.DeviceString );
            StringCchCopy( pMonitorInfoNew->strMonitorName, 128, dispdev2.DeviceString );
            
            if( dispdev.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP )
            {
                EnumDisplaySettings( dispdev.DeviceName, ENUM_CURRENT_SETTINGS, &devmode );
                if( dispdev.StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE )
                {
                    // For some reason devmode.dmPosition is not always (0, 0)
                    // for the primary display, so force it.
                    pMonitorInfoNew->rcScreen.left = 0;
                    pMonitorInfoNew->rcScreen.top = 0;
                }
                else
                {
                    pMonitorInfoNew->rcScreen.left = devmode.dmPosition.x;
                    pMonitorInfoNew->rcScreen.top = devmode.dmPosition.y;
                }
                pMonitorInfoNew->rcScreen.right = pMonitorInfoNew->rcScreen.left + devmode.dmPelsWidth;
                pMonitorInfoNew->rcScreen.bottom = pMonitorInfoNew->rcScreen.top + devmode.dmPelsHeight;
                pMonitorInfoNew->hMonitor = MonitorFromRect( &pMonitorInfoNew->rcScreen, MONITOR_DEFAULTTONULL );
            }
            m_dwNumMonitors++;
            if( m_dwNumMonitors == MAX_DISPLAYS )
                break;
        }
        iDevice++;
    }
}




//-----------------------------------------------------------------------------
// Name: CreateSaverWindow
// Desc: Register and create the appropriate window(s)
//-----------------------------------------------------------------------------
HRESULT CScreensaver::CreateSaverWindow()
{
    // Register an appropriate window class for the primary display
    WNDCLASS    cls;
    cls.hCursor        = LoadCursor( NULL, IDC_ARROW );
    cls.hIcon          = LoadIcon( m_hInstance, MAKEINTRESOURCE(IDI_MAIN_ICON) ); 
    cls.lpszMenuName   = NULL;
    cls.lpszClassName  = _T("BOINCPrimarySaverWndClass");
    cls.hbrBackground  = (HBRUSH) GetStockObject(BLACK_BRUSH);
    cls.hInstance      = m_hInstance; 
    cls.style          = CS_VREDRAW|CS_HREDRAW;
    cls.lpfnWndProc    = PrimarySaverProcStub;
    cls.cbWndExtra     = 0; 
    cls.cbClsExtra     = 0; 
    RegisterClass( &cls );

    // Register an appropriate window class for the secondary display(s)
    WNDCLASS    cls2;
    cls2.hCursor        = LoadCursor( NULL, IDC_ARROW );
    cls2.hIcon          = LoadIcon( m_hInstance, MAKEINTRESOURCE(IDI_MAIN_ICON) ); 
    cls2.lpszMenuName   = NULL;
    cls2.lpszClassName  = _T("BOINCGenericSaverWndClass");
    cls2.hbrBackground  = (HBRUSH) GetStockObject(BLACK_BRUSH);
    cls2.hInstance      = m_hInstance; 
    cls2.style          = CS_VREDRAW|CS_HREDRAW;
    cls2.lpfnWndProc    = GenericSaverProcStub;
    cls2.cbWndExtra     = 0; 
    cls2.cbClsExtra     = 0; 
    RegisterClass( &cls2 );

    // Create the window
    RECT rc;
    DWORD dwStyle;
    switch ( m_SaverMode )
    {
        case sm_preview:
            GetClientRect( m_hWndParent, &rc );
            dwStyle = WS_VISIBLE | WS_CHILD;
            AdjustWindowRect( &rc, dwStyle, FALSE );
            m_hWnd = CreateWindow( _T("BOINCPrimarySaverWndClass"), m_strWindowTitle, dwStyle, 
                                    rc.left, rc.top, rc.right-rc.left, rc.bottom-rc.top, 
                                    m_hWndParent, NULL, m_hInstance, this );
            m_Monitors[0].hWnd = m_hWnd;
            GetClientRect( m_hWnd, &m_rcRenderTotal );
            GetClientRect( m_hWnd, &m_rcRenderCurDevice );
            break;

        case sm_test:
            rc.left = rc.top = 50;
            rc.right = rc.left+600;
            rc.bottom = rc.top+400;
            dwStyle = WS_VISIBLE | WS_OVERLAPPED | WS_CAPTION | WS_MINIMIZEBOX | WS_SYSMENU;
            AdjustWindowRect( &rc, dwStyle, FALSE );
            m_hWnd = CreateWindow( _T("BOINCPrimarySaverWndClass"), m_strWindowTitle, dwStyle, 
                                   rc.left, rc.top, rc.right-rc.left, rc.bottom-rc.top, 
                                   NULL, NULL, m_hInstance, this );
            m_Monitors[0].hWnd = m_hWnd;
            GetClientRect( m_hWnd, &m_rcRenderTotal );
            GetClientRect( m_hWnd, &m_rcRenderCurDevice );
			SetTimer(m_hWnd, 2, 60000, NULL);
            break;

        case sm_full:
            dwStyle = WS_VISIBLE | WS_POPUP;
            m_hWnd = NULL;
            for( DWORD iMonitor = 0; iMonitor < m_dwNumMonitors; iMonitor++ )
            {
                INTERNALMONITORINFO* pMonitorInfo;
                pMonitorInfo = &m_Monitors[iMonitor];
				if( pMonitorInfo->hWnd == NULL )
				{
					if( pMonitorInfo->hMonitor == NULL )
						continue;
					rc = pMonitorInfo->rcScreen;
					if( 0 == iMonitor )
					{
						pMonitorInfo->hWnd = CreateWindowEx( WS_EX_TOPMOST, _T("BOINCPrimarySaverWndClass"), 
							m_strWindowTitle, dwStyle, rc.left, rc.top, rc.right - rc.left, 
							rc.bottom - rc.top, NULL, NULL, m_hInstance, this );
					}
					else
					{
						pMonitorInfo->hWnd = CreateWindowEx( WS_EX_TOPMOST, _T("BOINCGenericSaverWndClass"), 
							m_strWindowTitle, dwStyle, rc.left, rc.top, rc.right - rc.left, 
							rc.bottom - rc.top, NULL, NULL, m_hInstance, this );
					}
					if( pMonitorInfo->hWnd == NULL )
						return E_FAIL;
					
                    if( m_hWnd == NULL )
						m_hWnd = pMonitorInfo->hWnd;

					SetTimer(pMonitorInfo->hWnd, 2, 1000, NULL);
				}
            }
    }
    if ( m_hWnd == NULL )
        return E_FAIL;

    return S_OK;
}




//-----------------------------------------------------------------------------
// Name: DoSaver()
// Desc: Run the screensaver graphics - may be preview, test or full-on mode
//-----------------------------------------------------------------------------
HRESULT CScreensaver::DoSaver()
{
    // If we're in full on mode, and on 9x, then need to load the password DLL
    if ( m_SaverMode == sm_full && m_bIs9x )
    {
        // Only do this if the password is set - check registry:
        HKEY hKey; 
        if ( RegOpenKey( HKEY_CURRENT_USER , REGSTR_PATH_SCREENSAVE , &hKey ) == ERROR_SUCCESS ) 
        { 
            DWORD dwVal;
            DWORD dwSize = sizeof(dwVal); 
 
            if ( (RegQueryValueEx( hKey, REGSTR_VALUE_USESCRPASSWORD, NULL, NULL,
                                   (BYTE *)&dwVal, &dwSize ) == ERROR_SUCCESS) && dwVal ) 
            { 
                m_hPasswordDLL = LoadLibrary( _T("PASSWORD.CPL") );
                if ( m_hPasswordDLL )
                    m_VerifySaverPassword = (VERIFYPWDPROC)GetProcAddress( m_hPasswordDLL, _T("VerifyScreenSavePwd") );
                RegCloseKey( hKey );
            }
        }
    }

    // Flag as screensaver running if in full on mode
    if ( m_SaverMode == sm_full )
    {
        BOOL bUnused;
        SystemParametersInfo( SPI_SCREENSAVERRUNNING, TRUE, &bUnused, 0 );
    }


    // Message pump
    BOOL bGotMsg;
    MSG msg;
    msg.message = WM_NULL;
    while ( msg.message != WM_QUIT )
    {
        bGotMsg = PeekMessage( &msg, NULL, 0, 0, PM_REMOVE );
        if( bGotMsg )
        {
            TranslateMessage( &msg );
            DispatchMessage( &msg );
        }
        else
        {
            Sleep(10);
        }
    }

    return S_OK;
}




//-----------------------------------------------------------------------------
// Name: DoConfig()
// Desc: 
//-----------------------------------------------------------------------------
VOID CScreensaver::DoConfig()
{
    DialogBox( NULL, MAKEINTRESOURCE(DLG_CONFIG), m_hWndParent, ConfigureDialogProcStub );
}




//-----------------------------------------------------------------------------
// Name: PrimarySaverProc()
// Desc: Handle window messages for main screensaver windows (one per screen).
//-----------------------------------------------------------------------------
LRESULT CScreensaver::PrimarySaverProc( HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam )
{
    switch ( uMsg )
        {
        case WM_USER:
            // All initialization messages have gone through.  Allow
            // 500ms of idle time, then proceed with initialization.
            SetTimer( hWnd, 1, 500, NULL );
            break;

        case WM_TIMER:
			switch (wParam) 
			{ 
				case 1: 
					// Initial idle time is done, proceed with initialization.
					m_bWaitForInputIdle = FALSE;
					KillTimer( hWnd, 1 );
                    break;
				case 2:
                    // This timer is called once a second and handles drawing the error
                    // boxes as well as the initial 4 loops through of the update
                    // data timer.  It is necessary to call the UpdateErrorBox routine
                    // frequently or the error box will end up being drawn outside the 
                    // visible portion of the window.  Letting the update routine be called
                    // four times before setting up the update timer lets the request
                    // the initial display, then gives the core client enough time to
                    // choose an application or report an error.
                    if ( 3 >= m_dwBOINCTimerCounter ) m_dwBOINCTimerCounter++;
					if ( 4 == m_dwBOINCTimerCounter )
                    {
                        BOINCTRACE(_T("CScreensaver::PrimarySaverProc - Starting Update Timer\n"));
    					SetTimer(hWnd, 3, 30000, NULL);
                        m_dwBOINCTimerCounter++;
                    }
                    else
                    {
					    if ( 5 == m_dwBOINCTimerCounter )
                        {
                            if( m_bErrorMode )
					        {
                                BOINCTRACE(_T("CScreensaver::PrimarySaverProc - Updating Error Box\n"));
						        UpdateErrorBox();
					        }
                            break;
                        }
                    }
				case 3:
                    // Except for the initial startup sequence, this is run every
                    //   30 seconds.
                    HWND hwndBOINCGraphicsWindow = NULL;
                    HWND hwndForegroundWindow = NULL;
                    int  iReturnValue = 0;
                    int  iStatus = 0;


			        // Create a screen saver window on the primary display if the boinc client crashes
			        CreateSaverWindow();

                    BOINCTRACE(_T("CScreensaver::PrimarySaverProc - Start Status = '%d', BOINCCoreNotified = '%d', ErrorMode = '%d', ErrorCode = '%x'\n"), iStatus, m_bBOINCCoreNotified, m_bErrorMode, m_hrError);

                    iReturnValue = rpc.get_screensaver_mode( iStatus );
                    BOINCTRACE(_T("CScreensaver::PrimarySaverProc - get_screensaver_mode iReturnValue = '%d'\n"), iReturnValue);
                    if (0 != iReturnValue)
                    {
                    	// Attempt to reinitialize the RPC client and state
                        rpc.close();
                        rpc.init( NULL );
                        m_bResetCoreState = TRUE;

                        if (!m_bBOINCConfigChecked)
                        {
                            m_bBOINCConfigChecked = TRUE;
                            m_bBOINCStartupConfigured = IsConfigStartupBOINC();
                        }

			            if(m_bBOINCStartupConfigured)
			            {
				            m_bErrorMode = TRUE;
				            m_hrError = SCRAPPERR_BOINCNOTDETECTED;
			            }
			            else
			            {
				            m_bErrorMode = TRUE;
				            m_hrError = SCRAPPERR_BOINCNOTDETECTEDSTARTUP;
			            }

			            m_bBOINCCoreNotified = FALSE;
                    }
                    else
                    {
                        // Reset the error flags.
                   	    m_bErrorMode = FALSE;
    				    m_hrError = 0;

                        if (m_bBOINCCoreNotified)
                        {
                            switch (iStatus)
                            {
                                case SS_STATUS_ENABLED:
                                    hwndBOINCGraphicsWindow = FindWindow( BOINC_WINDOW_CLASS_NAME, NULL );
                                    if ( NULL != hwndBOINCGraphicsWindow )
                                    {
                                        hwndForegroundWindow = GetForegroundWindow();
                                        if ( hwndForegroundWindow != hwndBOINCGraphicsWindow )
                                        {
                                            BOINCTRACE(_T("CScreensaver::PrimarySaverProc - Graphics Window Detected but NOT the foreground window, bringing window to foreground.\n"));
                                            SetForegroundWindow(hwndBOINCGraphicsWindow);

                                            hwndForegroundWindow = GetForegroundWindow();
                                            if ( hwndForegroundWindow != hwndBOINCGraphicsWindow )
                                            {
                                                BOINCTRACE(_T("CScreensaver::PrimarySaverProc - Graphics Window Detected but NOT the foreground window, bringing window to foreground. (Final Try)\n"));
                                                // This may be needed on Windows 2000 or better machines
                                                DWORD dwComponents = BSM_APPLICATIONS;
                                                BroadcastSystemMessage( 
                                                    BSF_ALLOWSFW, 
                                                    &dwComponents,
                                                    WM_BOINCSFW,
                                                    NULL,
                                                    NULL
                                                );
                                            }
                                        }
                                    }
                                break;
                                case SS_STATUS_BLANKED:
                                    break;
                                case SS_STATUS_RESTARTREQUEST:
                                    m_bBOINCCoreNotified = FALSE;
                                    break;
                                case SS_STATUS_BOINCSUSPENDED:
       				                m_bErrorMode = TRUE;
    				                m_hrError = SCRAPPERR_BOINCSUSPENDED;
                                    break;
                                case SS_STATUS_NOAPPSEXECUTING:
       				                m_bErrorMode = TRUE;
    				                m_hrError = SCRAPPERR_BOINCNOAPPSEXECUTING;
                                    break;
                                case SS_STATUS_NOTGRAPHICSCAPABLE:
                                case SS_STATUS_NOGRAPHICSAPPSEXECUTING:
       				                m_bErrorMode = TRUE;
    				                m_hrError = SCRAPPERR_BOINCNOGRAPHICSAPPSEXECUTING;
                                    break;
                            }
                        }
                    }

                    BOINCTRACE(_T("CScreensaver::PrimarySaverProc - Checkpoint Status = '%d', BOINCCoreNotified = '%d', ErrorMode = '%d', ErrorCode = '%x'\n"), iStatus, m_bBOINCCoreNotified, m_bErrorMode, m_hrError);


                    // Lets try and get the current state of the CC
                    if ( m_bResetCoreState && m_bBOINCCoreNotified )
                    {
                        iReturnValue = rpc.get_state( state );
                        if ( 0 == iReturnValue )
                            m_bResetCoreState = FALSE;

                        BOINCTRACE(_T("CScreensaver::PrimarySaverProc - get_state iReturnValue = '%d'\n"), iReturnValue);
                    }


                    if( m_bErrorMode )
					{
                        BOINCTRACE(_T("CScreensaver::PrimarySaverProc - Updating Error Box Text\n"));
						UpdateErrorBoxText();
					}
                    else
                    {
                        if ( !m_bBOINCCoreNotified )
                        {
                            BOINCTRACE(_T("CScreensaver::PrimarySaverProc - Startup BOINC Screensaver\n"));
                            StartupBOINC();
                        }
                        else
                        {
                            if (SS_STATUS_QUIT == iStatus)
                            {
                                BOINCTRACE(_T("CScreensaver::PrimarySaverProc - Shutdown BOINC Screensaver\n"));
                                ShutdownSaver();
                            }
                        }
                    }

                    BOINCTRACE(_T("CScreensaver::PrimarySaverProc - End Status = '%d', BOINCCoreNotified = '%d', ErrorMode = '%d', ErrorCode = '%x'\n"), iStatus, m_bBOINCCoreNotified, m_bErrorMode, m_hrError);
            }
            break;

        case WM_DESTROY:
            if( m_SaverMode == sm_preview || m_SaverMode == sm_test )
                ShutdownSaver();
            break;

        case WM_SETCURSOR:
            if ( m_SaverMode == sm_full && !m_bCheckingSaverPassword )
            {
                // Hide cursor
                SetCursor( NULL );
                return TRUE;
            }
            break;

        case WM_PAINT:
        {
            // Show error message, if there is one
            PAINTSTRUCT ps;
            BeginPaint( hWnd, &ps );

            // In preview mode, just fill 
            // the preview window with black, and the BOINC icon. 
            if( !m_bErrorMode && m_SaverMode == sm_preview )
            {
                RECT rc;
                GetClientRect(hWnd,&rc);
				FillRect(ps.hdc, &rc, (HBRUSH)GetStockObject(BLACK_BRUSH) );
				DrawIcon(ps.hdc, (rc.right / 2) - 16, (rc.bottom / 2) - 16,
					LoadIcon( m_hInstance, MAKEINTRESOURCE(IDI_MAIN_ICON) ) );
            }
            else
            {
                DoPaint( hWnd, ps.hdc );
            }

            EndPaint( hWnd, &ps );
            return 0;
        }

        case WM_MOUSEMOVE:
            if( m_SaverMode != sm_test )
            {
                static INT xPrev = -1;
                static INT yPrev = -1;
                INT xCur = GET_X_LPARAM(lParam);
                INT yCur = GET_Y_LPARAM(lParam);
                if( xCur != xPrev || yCur != yPrev )
                {
                    xPrev = xCur;
                    yPrev = yCur;
                    m_dwSaverMouseMoveCount++;
                    if ( m_dwSaverMouseMoveCount > 5 )
                        InterruptSaver();
                }
            }
            break;

        case WM_KEYDOWN:
        case WM_LBUTTONDOWN:
        case WM_RBUTTONDOWN:
        case WM_MBUTTONDOWN:
            if( m_SaverMode != sm_test )
                InterruptSaver();
            break;

		case WM_NCACTIVATE:
		case WM_ACTIVATEAPP:
            return 0;
            break;

        case WM_POWERBROADCAST:
            if( wParam == PBT_APMSUSPEND && m_VerifySaverPassword == NULL )
                InterruptSaver();
            break;

        case WM_SYSCOMMAND: 
            if ( m_SaverMode == sm_full )
            {
                switch ( wParam )
                {
                    case SC_NEXTWINDOW:
                    case SC_PREVWINDOW:
                    case SC_SCREENSAVE:
                    case SC_CLOSE:
                        return FALSE;
                };
            }
            break;
    }

    BOINCTRACE(_T("PrimarySaverProc hWnd '%d' uMsg '%X' wParam '%d' lParam '%d'\n"), hWnd, uMsg, wParam, lParam);

    return DefWindowProc( hWnd, uMsg, wParam, lParam );
}




//-----------------------------------------------------------------------------
// Name: GenericSaverProc()
// Desc: Handle window messages for main screensaver windows (one per screen).
//-----------------------------------------------------------------------------
LRESULT CScreensaver::GenericSaverProc( HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam )
{
    switch ( uMsg )
        {
        case WM_USER:
            // All initialization messages have gone through.  Allow
            // 500ms of idle time, then proceed with initialization.
            SetTimer( hWnd, 1, 500, NULL );
            break;

        case WM_TIMER:
			switch (wParam) 
			{ 
				case 1: 
					// Initial idle time is done, proceed with initialization.
					m_bWaitForInputIdle = FALSE;
					KillTimer( hWnd, 1 );
					break; 
		 
				case 2: 
					if( m_bErrorMode )
					{
                        BOINCTRACE(_T("CScreensaver::GenericSaverProc - Updating Error Box\n"));
						UpdateErrorBox();
					}
					break; 
				case 3: 
					break; 
			}
            break;

        case WM_DESTROY:
            if( m_SaverMode == sm_preview || m_SaverMode == sm_test )
                ShutdownSaver();
            break;

        case WM_SETCURSOR:
            if ( m_SaverMode == sm_full && !m_bCheckingSaverPassword )
            {
                // Hide cursor
                SetCursor( NULL );
                return TRUE;
            }
            break;

        case WM_PAINT:
        {
            // Show error message, if there is one
            PAINTSTRUCT ps;
            BeginPaint( hWnd, &ps );

            // In preview mode, just fill 
            // the preview window with black, and the BOINC icon. 
            if( !m_bErrorMode && m_SaverMode == sm_preview )
            {
                RECT rc;
                GetClientRect(hWnd,&rc);
				FillRect(ps.hdc, &rc, (HBRUSH)GetStockObject(BLACK_BRUSH) );
				DrawIcon(ps.hdc, (rc.right / 2) - 16, (rc.bottom / 2) - 16,
					LoadIcon( m_hInstance, MAKEINTRESOURCE(IDI_MAIN_ICON) ) );
            }
            else
            {
                DoPaint( hWnd, ps.hdc );
            }

            EndPaint( hWnd, &ps );
            return 0;
        }

        case WM_MOUSEMOVE:
            if( m_SaverMode != sm_test )
            {
                static INT xPrev = -1;
                static INT yPrev = -1;
                INT xCur = GET_X_LPARAM(lParam);
                INT yCur = GET_Y_LPARAM(lParam);
                if( xCur != xPrev || yCur != yPrev )
                {
                    xPrev = xCur;
                    yPrev = yCur;
                    m_dwSaverMouseMoveCount++;
                    if ( m_dwSaverMouseMoveCount > 5 )
                        InterruptSaver();
                }
            }
            break;

        case WM_KEYDOWN:
        case WM_LBUTTONDOWN:
        case WM_RBUTTONDOWN:
        case WM_MBUTTONDOWN:
            if( m_SaverMode != sm_test )
                InterruptSaver();
            break;

        case WM_ACTIVATEAPP:
            if( wParam == FALSE && m_SaverMode != sm_test )
                InterruptSaver();
            break;

        case WM_POWERBROADCAST:
            if( wParam == PBT_APMSUSPEND && m_VerifySaverPassword == NULL )
                InterruptSaver();
            break;

        case WM_SYSCOMMAND: 
            if ( m_SaverMode == sm_full )
            {
                switch ( wParam )
                {
                    case SC_NEXTWINDOW:
                    case SC_PREVWINDOW:
                    case SC_SCREENSAVE:
                    case SC_CLOSE:
                        return FALSE;
                };
            }
            break;
    }

	BOINCTRACE(_T("GenericSaverProc hWnd '%d' uMsg '%X' wParam '%d' lParam '%d'\n"), hWnd, uMsg, wParam, lParam);

	return DefWindowProc( hWnd, uMsg, wParam, lParam );
}




//-----------------------------------------------------------------------------
// Name: ConfigureDialogProc()
// Desc: 
//-----------------------------------------------------------------------------
INT_PTR CScreensaver::ConfigureDialogProc(HWND hwnd,UINT msg,WPARAM wParam,LPARAM lParam) {
	DWORD screen_blank=0, blank_time=0;
	char buf[256];
	int retval;

	switch (msg) {
		case WM_INITDIALOG:
			// make sure you check return value of registry queries
			// in case the item in question doesn't happen to exist.
			retval = UtilGetRegKey( REG_BLANK_NAME, screen_blank );
			if ( retval < 0 ) { screen_blank=0; }
			CheckDlgButton( hwnd, IDC_BLANK, screen_blank );

			retval = UtilGetRegKey( REG_BLANK_TIME, blank_time );
			if ( retval < 0 ) { blank_time=0; }
			_ltot(blank_time, buf, 10);
			SetDlgItemText( hwnd, IDC_BLANK_TIME, buf );

			return TRUE;
		case WM_COMMAND:
			int id=LOWORD(wParam);
			if (id==IDOK) {

				screen_blank = ( IsDlgButtonChecked( hwnd, IDC_BLANK ) == BST_CHECKED );
				UtilSetRegKey( REG_BLANK_NAME, screen_blank );

				GetDlgItemText( hwnd, IDC_BLANK_TIME, buf, 256 );
				blank_time = atoi( buf );
				UtilSetRegKey( REG_BLANK_TIME, blank_time );

			}
			if ( id == IDOK || id == IDCANCEL )
				EndDialog( hwnd, id );
			break;
	}
	return FALSE;
}




//-----------------------------------------------------------------------------
// Name: PrimarySaverProcStub()
// Desc: This function forwards all window messages to SaverProc, which has
//       access to the "this" pointer.
//-----------------------------------------------------------------------------
LRESULT CALLBACK CScreensaver::PrimarySaverProcStub( HWND hWnd, UINT uMsg,
                                                 WPARAM wParam, LPARAM lParam )
{
    return gs_pScreensaver->PrimarySaverProc( hWnd, uMsg, wParam, lParam );
}




//-----------------------------------------------------------------------------
// Name: GenericSaverProcStub()
// Desc: This function forwards all window messages to SaverProc, which has
//       access to the "this" pointer.
//-----------------------------------------------------------------------------
LRESULT CALLBACK CScreensaver::GenericSaverProcStub( HWND hWnd, UINT uMsg,
                                                 WPARAM wParam, LPARAM lParam )
{
    return gs_pScreensaver->GenericSaverProc( hWnd, uMsg, wParam, lParam );
}




//-----------------------------------------------------------------------------
// Name: ConfigureDialogProcStub()
// Desc: 
//-----------------------------------------------------------------------------
INT_PTR CALLBACK CScreensaver::ConfigureDialogProcStub( HWND hwndDlg, UINT uMsg, WPARAM wParam, LPARAM lParam )
{
    return gs_pScreensaver->ConfigureDialogProc( hwndDlg, uMsg, wParam, lParam );
}




//////////
// Function:    UtilSetRegKey
// arguments:	name: name of key, keyval: where to store value of key
// returns:		int indicating error
// function:	reads string value in specified key
int CScreensaver::UtilSetRegKey(LPCTSTR name, DWORD value)
{
	LONG error;
	HKEY boinc_key;

	if ( m_bIs9x ) {
		error = RegCreateKeyEx( 
            HKEY_LOCAL_MACHINE, 
            _T("SOFTWARE\\Space Sciences Laboratory, U.C. Berkeley\\BOINC Screensaver"),  
			0,
            NULL,
            REG_OPTION_NON_VOLATILE,
            KEY_READ | KEY_WRITE,
            NULL,
            &boinc_key,
            NULL
        );
		if ( error != ERROR_SUCCESS ) return -1;
	} else {
		error = RegCreateKeyEx( 
            HKEY_CURRENT_USER,
            _T("SOFTWARE\\Space Sciences Laboratory, U.C. Berkeley\\BOINC Screensaver"),  
			0,
            NULL,
            REG_OPTION_NON_VOLATILE,
            KEY_READ | KEY_WRITE,
            NULL,
            &boinc_key,
            NULL
        );
		if ( error != ERROR_SUCCESS ) return -1;
	}

	error = RegSetValueEx( boinc_key, name, 0, REG_DWORD, (CONST BYTE *)&value, 4 );

	RegCloseKey( boinc_key );

	return 0;
}




//////////
// Function:    UtilGetRegKey
// arguments:	name: name of key, keyval: where to store value of key
// returns:		int indicating error
// function:	reads string value in specified key
int CScreensaver::UtilGetRegKey(LPCTSTR name, DWORD &keyval)
{
	LONG  error;
	DWORD type = REG_DWORD;
	DWORD size = sizeof( DWORD );
	TCHAR str[2048];
	DWORD value;
	HKEY  boinc_key;

    StringCbCat( str, sizeof(str) / sizeof(TCHAR), _T("SOFTWARE\\Space Sciences Laboratory, U.C. Berkeley\\BOINC Screensaver\\") );
    StringCbCat( str, sizeof(str) / sizeof(TCHAR), name );

	if ( m_bIs9x ) {
		error = RegOpenKeyEx( 
            HKEY_LOCAL_MACHINE, 
            _T("SOFTWARE\\Space Sciences Laboratory, U.C. Berkeley\\BOINC Screensaver"),  
			0, 
            KEY_ALL_ACCESS,
            &boinc_key
        );
		if ( error != ERROR_SUCCESS ) return -1;
	} else {
		error = RegOpenKeyEx(
            HKEY_CURRENT_USER,
            _T("SOFTWARE\\Space Sciences Laboratory, U.C. Berkeley\\BOINC Screensaver"),  
			0,
            KEY_ALL_ACCESS,
            &boinc_key
        );
		if ( error != ERROR_SUCCESS ) return -1;
	}

	error = RegQueryValueEx( boinc_key, name, NULL, &type, (BYTE *)&value, &size );

	keyval = value;

	RegCloseKey( boinc_key );

	if ( error != ERROR_SUCCESS ) return -1;

	return 0;
}




//////////
// Function:    UtilGetRegStartupStr
// arguments:	name: name of key, str: value of string to store
//				if str is empty, attepts to delete the key
// returns:		int indicating error
// function:	sets string value in specified key in windows startup dir
int CScreensaver::UtilGetRegStartupStr(LPCTSTR name, LPTSTR str)
{
	LONG error;
	DWORD type = REG_SZ;
	DWORD size = 128;
	HKEY boinc_key;

	*str = 0;

	if ( m_bIs9x ) {
		error = RegOpenKeyEx( 
            HKEY_LOCAL_MACHINE, 
            _T("Software\\Microsoft\\Windows\\CurrentVersion\\Run"),
			0, 
            KEY_ALL_ACCESS,
            &boinc_key
        );
		if ( error != ERROR_SUCCESS ) return -1;
	} else {
		error = RegOpenKeyEx( 
            HKEY_CURRENT_USER, 
            _T("Software\\Microsoft\\Windows\\CurrentVersion\\Run"),
			0, 
            KEY_ALL_ACCESS, 
            &boinc_key
        );
		if ( error != ERROR_SUCCESS ) return -1;
	}

	error = RegQueryValueEx( boinc_key, name, NULL, &type, (BYTE*)str, &size );

	RegCloseKey( boinc_key );

	if ( error != ERROR_SUCCESS ) return -1;

	return ERROR_SUCCESS;
}




//-----------------------------------------------------------------------------
// Name: ShutdownSaver()
// Desc: 
//-----------------------------------------------------------------------------
VOID CScreensaver::ShutdownSaver()
{
    // Unflag screensaver running if in full on mode
    if ( m_SaverMode == sm_full )
    {
        BOOL bUnused;
        SystemParametersInfo( SPI_SCREENSAVERRUNNING, FALSE, &bUnused, 0 );
    }

    // Unload the password DLL (if we loaded it)
    if ( m_hPasswordDLL != NULL )
    {
        FreeLibrary( m_hPasswordDLL );
        m_hPasswordDLL = NULL;
    }

	ShutdownBOINC();

    // Post message to drop out of message loop
    PostQuitMessage( 0 );
}




//-----------------------------------------------------------------------------
// Name: InterruptSaver()
// Desc: A message was received (mouse move, keydown, etc.) that may mean
//       the screen saver should show the password dialog and/or shut down.
//-----------------------------------------------------------------------------
VOID CScreensaver::InterruptSaver()
{
    BOOL bPasswordOkay = FALSE;

    if( m_SaverMode == sm_test ||
        m_SaverMode == sm_full && !m_bCheckingSaverPassword )
    {
        if( m_bIs9x && m_SaverMode == sm_full )
        {
            // If no VerifyPassword function, then no password is set 
            // or we're not on 9x. 
            if ( m_VerifySaverPassword != NULL )
            {
                m_bCheckingSaverPassword = TRUE;

                bPasswordOkay = m_VerifySaverPassword( m_hWnd );

                m_bCheckingSaverPassword = FALSE;

                if ( !bPasswordOkay )
                {
                    // Back to screen saving...
                    SetCursor( NULL );
                    m_dwSaverMouseMoveCount = 0;

                    return;
                }
            }
        }
        ShutdownSaver();
    }
}




//-----------------------------------------------------------------------------
// Name: UpdateErrorBox()
// Desc: Update the box that shows the error message
//-----------------------------------------------------------------------------
VOID CScreensaver::UpdateErrorBox()
{
    INTERNALMONITORINFO* pMonitorInfo;
    HWND hwnd;
    RECT rcBounds;
    static DWORD dwTimeLast = 0;
    DWORD dwTimeNow;
    FLOAT fTimeDelta;


    // Update timing to determine how much to move error box
    if( dwTimeLast == 0 )
        dwTimeLast = timeGetTime();
    dwTimeNow = timeGetTime();
    fTimeDelta = (FLOAT)(dwTimeNow - dwTimeLast) / 1000.0f;
    dwTimeLast = dwTimeNow;

    for( DWORD iMonitor = 0; iMonitor < m_dwNumMonitors; iMonitor++ )
    {
        pMonitorInfo = &m_Monitors[iMonitor];
        hwnd = pMonitorInfo->hWnd;
        if( hwnd == NULL )
            continue;
        if( m_SaverMode == sm_full )
        {
            rcBounds = pMonitorInfo->rcScreen;
            ScreenToClient( hwnd, (POINT*)&rcBounds.left );
            ScreenToClient( hwnd, (POINT*)&rcBounds.right );
        }
        else
        {
            rcBounds = m_rcRenderTotal;
        }

        if( pMonitorInfo->widthError == 0 )
        {
            if( m_SaverMode == sm_preview )                
            {
                pMonitorInfo->widthError = (float) (rcBounds.right - rcBounds.left);
                pMonitorInfo->heightError = (float) (rcBounds.bottom - rcBounds.top);
                pMonitorInfo->xError = 0.0f;
                pMonitorInfo->yError = 0.0f;
                pMonitorInfo->xVelError = 0.0f;
                pMonitorInfo->yVelError = 0.0f;
                InvalidateRect( hwnd, NULL, FALSE );    // Invalidate the hwnd so it gets drawn
                UpdateWindow( hwnd );
            }
            else
            {
                pMonitorInfo->widthError = 500;
                pMonitorInfo->heightError = 154;
                pMonitorInfo->xError = (rcBounds.right + rcBounds.left - pMonitorInfo->widthError) / 2.0f;
                pMonitorInfo->yError = (rcBounds.bottom + rcBounds.top - pMonitorInfo->heightError) / 2.0f;
                pMonitorInfo->xVelError = (rcBounds.right - rcBounds.left) / 10.0f;
                pMonitorInfo->yVelError = (rcBounds.bottom - rcBounds.top) / 20.0f;
            }
        }
        else
        {
            if( m_SaverMode != sm_preview )
            {
                RECT rcOld;
                RECT rcNew;

                SetRect( &rcOld, (INT)pMonitorInfo->xError, (INT)pMonitorInfo->yError,
                    (INT)(pMonitorInfo->xError + pMonitorInfo->widthError),
                    (INT)(pMonitorInfo->yError + pMonitorInfo->heightError) );

                // Update rect velocity
                if( (pMonitorInfo->xError + pMonitorInfo->xVelError * fTimeDelta + 
                    pMonitorInfo->widthError > rcBounds.right && pMonitorInfo->xVelError > 0.0f) ||
                    (pMonitorInfo->xError + pMonitorInfo->xVelError * fTimeDelta < 
                    rcBounds.left && pMonitorInfo->xVelError < 0.0f) )
                {
                    pMonitorInfo->xVelError = -pMonitorInfo->xVelError;
                }
                if( (pMonitorInfo->yError + pMonitorInfo->yVelError * fTimeDelta + 
                    pMonitorInfo->heightError > rcBounds.bottom && pMonitorInfo->yVelError > 0.0f) ||
                    (pMonitorInfo->yError + pMonitorInfo->yVelError * fTimeDelta < 
                    rcBounds.top && pMonitorInfo->yVelError < 0.0f) )
                {
                    pMonitorInfo->yVelError = -pMonitorInfo->yVelError;
                }
                // Update rect position
                pMonitorInfo->xError += pMonitorInfo->xVelError * fTimeDelta;
                pMonitorInfo->yError += pMonitorInfo->yVelError * fTimeDelta;
            
                SetRect( &rcNew, (INT)pMonitorInfo->xError, (INT)pMonitorInfo->yError,
                    (INT)(pMonitorInfo->xError + pMonitorInfo->widthError),
                    (INT)(pMonitorInfo->yError + pMonitorInfo->heightError) );

                if( rcOld.left != rcNew.left || rcOld.top != rcNew.top )
                {
                    InvalidateRect( hwnd, &rcOld, FALSE );    // Invalidate old rect so it gets erased
                    InvalidateRect( hwnd, &rcNew, FALSE );    // Invalidate new rect so it gets drawn
                    UpdateWindow( hwnd );
                }
            }
        }
    }
}




//-----------------------------------------------------------------------------
// Name: UpdateErrorBoxText()
// Desc: Update the error message
//-----------------------------------------------------------------------------
VOID CScreensaver::UpdateErrorBoxText()
{
    RESULTS  results;
    PROJECT* pProject;
    TCHAR    szBuffer[256];
    bool     bIsActive       = false;
    bool     bIsExecuting    = false;
    bool     bIsDownloaded   = false;
    int      iResultCount    = 0;
    int      iIndex          = 0;
    float    fProgress       = 0;


    // Load error string
    GetTextForError( m_hrError, m_szError, sizeof(m_szError) / sizeof(TCHAR) );
    if( SCRAPPERR_BOINCNOGRAPHICSAPPSEXECUTING == m_hrError )
    {
        if( 0 == rpc.get_results( results ) )
        {
            iResultCount = results.results.size();
            for ( iIndex = 0; iIndex < iResultCount; iIndex++ )
            {
                bIsDownloaded = ( RESULT_FILES_DOWNLOADED == results.results.at(iIndex)->state );
                bIsActive     = ( results.results.at(iIndex)->active_task );
                bIsExecuting  = ( CPU_SCHED_SCHEDULED == results.results.at(iIndex)->scheduler_state );
                if ( !( bIsActive ) || !( bIsDownloaded ) || !( bIsExecuting ) ) continue;

                pProject = state.lookup_project( results.results.at( iIndex )->project_url );
                if ( NULL != pProject )
                {
                    StringCbPrintf( szBuffer, sizeof(szBuffer) / sizeof(TCHAR),
                        _T("%s: %.2f%%\n"),
                        pProject->project_name.c_str(),
                        results.results.at(iIndex)->fraction_done * 100 
                    );

                    StringCbCat( m_szError, sizeof(m_szError) / sizeof(TCHAR), szBuffer );
                }
                else
                {
                    m_bResetCoreState = TRUE;
                }
            }
            m_szError[ sizeof(m_szError) -1 ] = '\0';
            BOINCTRACE(_T("CScreensaver::UpdateErrorBoxText - Updated Text '%s'\n"), m_szError);
        }
    }
}




//-----------------------------------------------------------------------------
// Name: GetTextForError()
// Desc: Translate an HRESULT error code into a string that can be displayed
//       to explain the error.  A class derived from CD3DScreensaver can 
//       provide its own version of this function that provides app-specific
//       error translation instead of or in addition to calling this function.
//       This function returns TRUE if a specific error was translated, or
//       FALSE if no specific translation for the HRESULT was found (though
//       it still puts a generic string into pszError).
//-----------------------------------------------------------------------------
BOOL CScreensaver::GetTextForError( HRESULT hr, TCHAR* pszError, DWORD dwNumChars )
{
    const DWORD dwErrorMap[][2] = 
    {
    //  HRESULT, stringID
        E_FAIL, IDS_ERR_GENERIC,
        E_OUTOFMEMORY, IDS_ERR_OUTOFMEMORY,
		SCRAPPERR_BOINCNOTDETECTED, IDS_ERR_BOINCNOTDETECTED,
		SCRAPPERR_BOINCNOTDETECTEDSTARTUP, IDS_ERR_BOINCNOTDETECTEDSTARTUP,
		SCRAPPERR_BOINCSUSPENDED, IDS_ERR_BOINCSUSPENDED,
		SCRAPPERR_BOINCNOAPPSEXECUTING, IDS_ERR_BOINCNOAPPSEXECUTING,
		SCRAPPERR_BOINCNOGRAPHICSAPPSEXECUTING, IDS_ERR_BOINCNOGRAPHICSAPPSEXECUTING,
		SCRAPPERR_NOPREVIEW, IDS_ERR_NOPREVIEW
    };
    const DWORD dwErrorMapSize = sizeof(dwErrorMap) / sizeof(DWORD[2]);

    DWORD iError;
    DWORD resid = 0;

    for( iError = 0; iError < dwErrorMapSize; iError++ )
    {
        if( hr == (HRESULT)dwErrorMap[iError][0] )
        {
            resid = dwErrorMap[iError][1];
        }
    }
    if( resid == 0 )
    {
        resid = IDS_ERR_GENERIC;
    }

    LoadString( NULL, resid, pszError, dwNumChars );

    if( resid == IDS_ERR_GENERIC )
        return FALSE;
    else
        return TRUE;
}




//-----------------------------------------------------------------------------
// Name: DoPaint()
// Desc: 
//-----------------------------------------------------------------------------
VOID CScreensaver::DoPaint(HWND hwnd, HDC hdc)
{
    HMONITOR hMonitor = MonitorFromWindow( hwnd, MONITOR_DEFAULTTONEAREST );
    INTERNALMONITORINFO* pMonitorInfo;
    for( DWORD iMonitor = 0; iMonitor < m_dwNumMonitors; iMonitor++)
    {
        pMonitorInfo = &m_Monitors[iMonitor];
        if( pMonitorInfo->hMonitor == hMonitor )
            break;
    }

    if( iMonitor == m_dwNumMonitors )
        return;

    // Draw the error message box
    RECT    rc;
    RECT    rc2;
    RECT    rcOrginal;
	int		iTextHeight;

	static HBRUSH	hbrushBlack = (HBRUSH)GetStockObject( BLACK_BRUSH );
	static HBRUSH	hbrushRed = (HBRUSH)CreateSolidBrush( RGB(255,0,0) );
	static HBITMAP  hbmp = LoadBitmap( m_hInstance, MAKEINTRESOURCE(IDB_BOINCSPLAT) );


	SetRect( &rc, (INT)pMonitorInfo->xError, (INT)pMonitorInfo->yError,
        (INT)(pMonitorInfo->xError + pMonitorInfo->widthError),
        (INT)(pMonitorInfo->yError + pMonitorInfo->heightError) );


	SetRect( &rcOrginal, (INT)pMonitorInfo->xError, (INT)pMonitorInfo->yError,
        (INT)(pMonitorInfo->xError + pMonitorInfo->widthError),
        (INT)(pMonitorInfo->yError + pMonitorInfo->heightError) );


	// Draw the background as black, and put a frame around it that it red.
	FillRect(hdc, &rc, hbrushBlack);
	FrameRect(hdc, &rc, hbrushRed);

    // Draw the bitmap rectangle and copy the bitmap into 
    // it. the bitmap is centered in the rectangle by adding 2
	// to the left and top coordinates of the bitmap rectangle,
	// and subtracting 4 from the right and bottom coordinates.
	DrawTransparentBitmap(hdc, hbmp, (rc.left + 2), (rc.top + 2), RGB(129, 129, 129));
	rc.left += 166;

	// Draw text in the center of the frame
	SetBkColor(hdc, RGB(0,0,0));           // Black
	SetTextColor(hdc, RGB(255,255,255));   // Red

	rc2 = rc;
    iTextHeight = DrawText(hdc, m_szError, -1, &rc, DT_CENTER | DT_CALCRECT );
	rc = rc2;
    rc2.top = (rc.bottom + rc.top - iTextHeight) / 2;
    DrawText(hdc, m_szError, -1, &rc2, DT_CENTER );


    // Erase everywhere except the error message box
    ExcludeClipRect( hdc, rcOrginal.left, rcOrginal.top, rcOrginal.right, rcOrginal.bottom );
    rc = pMonitorInfo->rcScreen;
    ScreenToClient( hwnd, (POINT*)&rc.left );
    ScreenToClient( hwnd, (POINT*)&rc.right );
    FillRect(hdc, &rc, hbrushBlack );

}




//-----------------------------------------------------------------------------
// Name: ChangePassword()
// Desc:
//-----------------------------------------------------------------------------
VOID CScreensaver::ChangePassword()
{
    // Load the password change DLL
    HINSTANCE mpr = LoadLibrary( _T("MPR.DLL") );

    if ( mpr != NULL )
    {
        // Grab the password change function from it
        typedef DWORD (PASCAL *PWCHGPROC)( LPCSTR, HWND, DWORD, LPVOID );
        PWCHGPROC pwd = (PWCHGPROC)GetProcAddress( mpr, "PwdChangePasswordA" );

        // Do the password change
        if ( pwd != NULL )
            pwd( "SCRSAVE", m_hWndParent, 0, NULL );

        // Free the library
        FreeLibrary( mpr );
    }
}




//-----------------------------------------------------------------------------
// Name: DisplayErrorMsg()
// Desc: Displays error messages in a message box
//-----------------------------------------------------------------------------
HRESULT CScreensaver::DisplayErrorMsg( HRESULT hr )
{
    TCHAR strMsg[512];

    GetTextForError( hr, strMsg, 512 );

    MessageBox( m_hWnd, strMsg, m_strWindowTitle, MB_ICONERROR | MB_OK );

    return hr;
}




//-----------------------------------------------------------------------------
// Name: DrawTransparentBitmap()
// Desc: Draws a bitmap on the screen with a transparent background.
//         Code orginally from Microsoft Knowledge Base Article - 79212
//-----------------------------------------------------------------------------
void CScreensaver::DrawTransparentBitmap(HDC hdc, HBITMAP hBitmap, LONG xStart, LONG yStart, COLORREF cTransparentColor)
{
   BITMAP     bm;
   COLORREF   cColor;
   HBITMAP    bmAndBack, bmAndObject, bmAndMem, bmSave;
   HBITMAP    bmBackOld, bmObjectOld, bmMemOld, bmSaveOld;
   HDC        hdcMem, hdcBack, hdcObject, hdcTemp, hdcSave;
   POINT      ptSize;

   hdcTemp = CreateCompatibleDC(hdc);
   SelectObject(hdcTemp, hBitmap);   // Select the bitmap

   GetObject(hBitmap, sizeof(BITMAP), (LPSTR)&bm);
   ptSize.x = bm.bmWidth;            // Get width of bitmap
   ptSize.y = bm.bmHeight;           // Get height of bitmap
   DPtoLP(hdcTemp, &ptSize, 1);      // Convert from device

                                     // to logical points

   // Create some DCs to hold temporary data.
   hdcBack   = CreateCompatibleDC(hdc);
   hdcObject = CreateCompatibleDC(hdc);
   hdcMem    = CreateCompatibleDC(hdc);
   hdcSave   = CreateCompatibleDC(hdc);

   // Create a bitmap for each DC. DCs are required for a number of
   // GDI functions.

   // Monochrome DC
   bmAndBack   = CreateBitmap(ptSize.x, ptSize.y, 1, 1, NULL);

   // Monochrome DC
   bmAndObject = CreateBitmap(ptSize.x, ptSize.y, 1, 1, NULL);

   bmAndMem    = CreateCompatibleBitmap(hdc, ptSize.x, ptSize.y);
   bmSave      = CreateCompatibleBitmap(hdc, ptSize.x, ptSize.y);

   // Each DC must select a bitmap object to store pixel data.
   bmBackOld   = (HBITMAP)SelectObject(hdcBack, bmAndBack);
   bmObjectOld = (HBITMAP)SelectObject(hdcObject, bmAndObject);
   bmMemOld    = (HBITMAP)SelectObject(hdcMem, bmAndMem);
   bmSaveOld   = (HBITMAP)SelectObject(hdcSave, bmSave);

   // Set proper mapping mode.
   SetMapMode(hdcTemp, GetMapMode(hdc));

   // Save the bitmap sent here, because it will be overwritten.
   BitBlt(hdcSave, 0, 0, ptSize.x, ptSize.y, hdcTemp, 0, 0, SRCCOPY);

   // Set the background color of the source DC to the color.
   // contained in the parts of the bitmap that should be transparent
   cColor = SetBkColor(hdcTemp, cTransparentColor);

   // Create the object mask for the bitmap by performing a BitBlt
   // from the source bitmap to a monochrome bitmap.
   BitBlt(hdcObject, 0, 0, ptSize.x, ptSize.y, hdcTemp, 0, 0, SRCCOPY);

   // Set the background color of the source DC back to the original
   // color.
   SetBkColor(hdcTemp, cColor);

   // Create the inverse of the object mask.
   BitBlt(hdcBack, 0, 0, ptSize.x, ptSize.y, hdcObject, 0, 0, NOTSRCCOPY);

   // Copy the background of the main DC to the destination.
   BitBlt(hdcMem, 0, 0, ptSize.x, ptSize.y, hdc, xStart, yStart, SRCCOPY);

   // Mask out the places where the bitmap will be placed.
   BitBlt(hdcMem, 0, 0, ptSize.x, ptSize.y, hdcObject, 0, 0, SRCAND);

   // Mask out the transparent colored pixels on the bitmap.
   BitBlt(hdcTemp, 0, 0, ptSize.x, ptSize.y, hdcBack, 0, 0, SRCAND);

   // XOR the bitmap with the background on the destination DC.
   BitBlt(hdcMem, 0, 0, ptSize.x, ptSize.y, hdcTemp, 0, 0, SRCPAINT);

   // Copy the destination to the screen.
   BitBlt(hdc, xStart, yStart, ptSize.x, ptSize.y, hdcMem, 0, 0, SRCCOPY);

   // Place the original bitmap back into the bitmap sent here.
   BitBlt(hdcTemp, 0, 0, ptSize.x, ptSize.y, hdcSave, 0, 0, SRCCOPY);

   // Delete the memory bitmaps.
   DeleteObject(SelectObject(hdcBack, bmBackOld));
   DeleteObject(SelectObject(hdcObject, bmObjectOld));
   DeleteObject(SelectObject(hdcMem, bmMemOld));
   DeleteObject(SelectObject(hdcSave, bmSaveOld));

   // Delete the memory DCs.
   DeleteDC(hdcMem);
   DeleteDC(hdcBack);
   DeleteDC(hdcObject);
   DeleteDC(hdcSave);
   DeleteDC(hdcTemp);
}




//-----------------------------------------------------------------------------
// Name: IsConfigStatupBOINC()
// Desc: Determine if BOINC is configured to automatically start at logon/startup.
//-----------------------------------------------------------------------------

// Define dynamically linked to function
typedef HRESULT (STDAPICALLTYPE* MYSHGETFOLDERPATH)(HWND hwnd, int csidl, HANDLE hToken, DWORD dwFlags, LPSTR pszPath);

BOOL CScreensaver::IsConfigStartupBOINC()
{
	BOOL				bRetVal;
	BOOL				bCheckFileExists;
	TCHAR				szBuffer[MAX_PATH];
	HANDLE				hFileHandle;
    HMODULE				hShell32;
	MYSHGETFOLDERPATH	pfnMySHGetFolderPath = NULL;


	// Lets set the default value to FALSE
	bRetVal = FALSE;

	// Attempt to link to dynamic function if it exists
    hShell32 = LoadLibrary(_T("SHELL32.DLL"));
	if ( NULL != hShell32 )
		pfnMySHGetFolderPath = (MYSHGETFOLDERPATH) GetProcAddress(hShell32, _T("SHGetFolderPathA"));


	// Now lets begin looking in the registry
	if (ERROR_SUCCESS == UtilGetRegStartupStr(REG_STARTUP_NAME, szBuffer))
	{
		bRetVal = TRUE;
	}
	else
	{
		// It could be in the global startup group
		ZeroMemory( szBuffer, sizeof(szBuffer) );
		bCheckFileExists = FALSE;
		if ( NULL != pfnMySHGetFolderPath )
		{
			if (SUCCEEDED((pfnMySHGetFolderPath)(NULL, CSIDL_STARTUP|CSIDL_FLAG_CREATE, NULL, SHGFP_TYPE_CURRENT, szBuffer)))
			{
				BOINCTRACE(_T("IsConfigStartupBOINC: pfnMySHGetFolderPath - CSIDL_STARTUP - '%s'\n"), szBuffer);
				if (SUCCEEDED(StringCchCatN(szBuffer, sizeof(szBuffer), BOINC_SHORTCUT_NAME, sizeof(BOINC_SHORTCUT_NAME))))
				{
					BOINCTRACE(_T("IsConfigStartupBOINC: Final pfnMySHGetFolderPath - CSIDL_STARTUP - '%s'\n"), szBuffer);
					bCheckFileExists = TRUE;
				}
				else
				{
					BOINCTRACE(_T("IsConfigStartupBOINC: FAILED pfnMySHGetFolderPath - CSIDL_STARTUP Append Operation\n"));
				}
			}
			else
			{
				BOINCTRACE(_T("IsConfigStartupBOINC: FAILED pfnMySHGetFolderPath - CSIDL_STARTUP\n"));
			}
		}


		if (bCheckFileExists)
		{
			hFileHandle = CreateFile(
				szBuffer,
				GENERIC_READ,
				FILE_SHARE_READ,
				NULL,
				OPEN_EXISTING,
				FILE_ATTRIBUTE_NORMAL,
				NULL);

			if (INVALID_HANDLE_VALUE != hFileHandle)
			{
				BOINCTRACE(_T("IsConfigStartupBOINC: CreateFile returned a valid handle '%d'\n"), hFileHandle);
				CloseHandle(hFileHandle);
				bRetVal = TRUE;
			}
			else
			{
				BOINCTRACE(_T("IsConfigStartupBOINC: CreateFile returned INVALID_HANDLE_VALUE - GetLastError() '%d'\n"), GetLastError());

				// It could be in the global startup group
        		ZeroMemory( szBuffer, sizeof(szBuffer) );
				bCheckFileExists = FALSE;
				if ( NULL != pfnMySHGetFolderPath )
				{
					if (SUCCEEDED((pfnMySHGetFolderPath)(NULL, CSIDL_COMMON_STARTUP|CSIDL_FLAG_CREATE, NULL, SHGFP_TYPE_CURRENT, szBuffer)))
					{
						BOINCTRACE(_T("IsConfigStartupBOINC: pfnMySHGetFolderPath - CSIDL_COMMON_STARTUP - '%s'\n"), szBuffer);
						if (SUCCEEDED(StringCchCatN(szBuffer, sizeof(szBuffer), BOINC_SHORTCUT_NAME, sizeof(BOINC_SHORTCUT_NAME))))
						{
							BOINCTRACE(_T("IsConfigStartupBOINC: Final pfnMySHGetFolderPath - CSIDL_COMMON_STARTUP - '%s'\n"), szBuffer);
							bCheckFileExists = TRUE;
						}
						else
						{
							BOINCTRACE(_T("IsConfigStartupBOINC: FAILED pfnMySHGetFolderPath - CSIDL_COMMON_STARTUP Append Operation\n"));
						}
					}
					else
					{
						BOINCTRACE(_T("IsConfigStartupBOINC: FAILED pfnMySHGetFolderPath - CSIDL_COMMON_STARTUP\n"));
					}
				}


				if (bCheckFileExists)
				{
					hFileHandle = CreateFile(
						szBuffer,
						GENERIC_READ,
						FILE_SHARE_READ,
						NULL,
						OPEN_EXISTING,
						FILE_ATTRIBUTE_NORMAL,
						NULL);

					if (INVALID_HANDLE_VALUE != hFileHandle)
					{
						BOINCTRACE(_T("IsConfigStartupBOINC: CreateFile returned a valid handle '%d'\n"), hFileHandle);
						CloseHandle(hFileHandle);
						bRetVal = TRUE;
					}
					else
					{
						BOINCTRACE(_T("IsConfigStartupBOINC: CreateFile returned INVALID_HANDLE_VALUE - GetLastError() '%d'\n"), GetLastError());
					}
				}
			}
		}
	}


	// Free the dynamically linked to library
	FreeLibrary(hShell32);


	BOINCTRACE(_T("IsConfigStartupBOINC: Returning '%d'\n"), bRetVal);
	return bRetVal;
}


const char *BOINC_RCSID_116268c72f = "$Id$";
