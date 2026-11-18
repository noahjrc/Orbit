// Orbit, an Xbox 360 homebrew launcher. Copyright (c) 2026 Noah Colbourne.
// Licensed under the GNU General Public License v3.0 or later; see LICENSE.

// OrbitTest.cpp : Orbit launcher, entry point and main loop.
//
// Orbit is a port of orbit-prototype.html (in the repo root), a three.js page that was
// used to design the look. Comments that say "the prototype" mean that page, and
// names like uTime or .gameinfo are the names it uses, so the two can be compared.
//
// Each frame: read the controllers, update whichever view is open (the menu ring, a
// shelf, FILES or SETTINGS), then draw. The 3D scene (backdrop, stars, orb, ring) is
// drawn in linear light with bloom (Post.cpp); the front menu panels, shelf cases and
// HUD are drawn on top after bloom so the glow never washes them out.
//
// Main menu: left / right turns the ring, A pulses the orb and opens the item's view.
// B goes back out of a view and does nothing on the main menu.
//

#include "stdafx.h"
#include "Screen.h"
#include "Post.h"
#include "Backdrop.h"
#include "Orb.h"
#include "Ring.h"
#include "Hud.h"
#include "Library.h"
#include "Files.h"
#include "Drives.h"
#include "Settings.h"
#include "Sound.h"
#include "Scan.h"
#include "Profile.h"

//-------------------------------------------------------------------------------------
// Constants from the prototype
//-------------------------------------------------------------------------------------
static const float RING_RADIUS = 3.5f;    // radius of the menu ring the camera keeps in frame
static const float CAMERA_FOV  = 50.0f;   // vertical, degrees

//-------------------------------------------------------------------------------------
// Time, kept as a quadword so it doesn't lose precision over a long session
//-------------------------------------------------------------------------------------
struct TimeInfo
{
    LARGE_INTEGER qwLast;
    float         fSecsPerTick;
    float         fElapsed;      // seconds since start (drives the camera drift)
    float         fOrbTime;      // seconds fed to the orb's noise (the prototype's uTime)
};

//-------------------------------------------------------------------------------------
// Global variables
//-------------------------------------------------------------------------------------
D3DDevice* g_pd3dDevice = NULL;   // Our rendering device
ScreenInfo g_Screen;
TimeInfo   g_Time;
float      g_fPulse   = 0.0f;     // 1 on select, decays to 0
float      g_fCamDist = 8.2f;     // set from the aspect in InitScreen()
float      g_fCamDistLib = 6.5f;  // camera distance while a shelf is open
float      g_fCamZ    = 8.2f;     // eases between the two (damp 4)
float      g_fRingScale = 1.0f;   // ring tucks away while a shelf is open (damp 7)
float      g_fOrbY    = 0.0f;     // orb floats up to 0.6 while a shelf is open (damp 5)
float      g_fOpenIn  = -1.0f;    // seconds until the selected shelf opens; < 0 = none

// Buttons held on each pad last frame. Starts as "everything held" so a button still
// down from whatever launched Orbit must be released before it counts.
WORD g_wPrevButtons[XUSER_MAX_COUNT];
WORD g_wArmed[XUSER_MAX_COUNT];         // buttons seen released since launch


//-------------------------------------------------------------------------------------
// Name: ExitToDefaultApp()
// Desc: Leaves Orbit when it can't start. When Orbit is Dashlaunch's Default= app,
//       Dashlaunch also sends "go to the dashboard" to Orbit, so the dashboard
//       keyword would just restart a broken Orbit forever. So try Aurora at its usual
//       place first, then the dashboard. XLaunchNewImage only returns if it failed.
//-------------------------------------------------------------------------------------
DECLSPEC_NORETURN void ExitToDefaultApp()
{
    Drives_Init();   // links Usb0:
    XLaunchNewImage( "Usb0:\\Apps\\Aurora\\Aurora.xex", 0 );
    XLaunchNewImage( XLAUNCH_KEYWORD_DASH, 0 );
    for( ;; )
        Sleep( 1000 );
}


//-------------------------------------------------------------------------------------
// Name: InitScreen()
// Desc: Chooses the back buffer for the current video mode. The 360's scaler
//       stretches the back buffer to fill whatever it outputs, so the buffer's
//       shape should match the TV's shape to keep pixels square:
//         widescreen (1080p, 720p, 480p wide)  -> 1280x720, 16:9
//         4:3 SD (480p/480i normal)            -> 640x480,  4:3
//         4:3 HD (VGA 1024x768 and the like)   -> 960x720,  4:3
//       1080p still renders at 720 and is upscaled by the scaler, as most 360
//       titles do: a 1920x1080 target does not fit in EDRAM without tiling.
//-------------------------------------------------------------------------------------
void InitScreen()
{
    XVIDEO_MODE VideoMode;
    ZeroMemory( &VideoMode, sizeof(VideoMode) );
    XGetVideoMode( &VideoMode );

    ScreenInfo& s = g_Screen;
    s.bWidescreen   = VideoMode.fIsWideScreen;
    s.DisplayHeight = VideoMode.dwDisplayHeight;

    if( s.bWidescreen )
    {
        s.Width = 1280; s.Height = 720;
        s.DisplayAspect = 16.0f / 9.0f;
    }
    else
    {
        if( VideoMode.fIsHiDef ) { s.Width = 960; s.Height = 720; }
        else                     { s.Width = 640; s.Height = 480; }
        s.DisplayAspect = 4.0f / 3.0f;
    }

    s.PixelAspect = s.DisplayAspect * (float)s.Height / (float)s.Width;
    s.Unit        = (float)s.Height / 480.0f;

    LONG lInsetX = (LONG)( s.Width  * 0.05f );
    LONG lInsetY = (LONG)( s.Height * 0.05f );
    s.SafeRect.left   = lInsetX;
    s.SafeRect.top    = lInsetY;
    s.SafeRect.right  = (LONG)s.Width  - lInsetX;
    s.SafeRect.bottom = (LONG)s.Height - lInsetY;

    // Prototype resize(): keep the whole ring plus half a selected panel in frame
    // horizontally, never closer than 8.2 so the front panel clears the bottom bar.
    // 16:9 lands on 8.2; 4:3 pulls back to about 8.7.
    float fTanH = tanf( XMConvertToRadians( CAMERA_FOV * 0.5f ) ) * s.DisplayAspect;
    g_fCamDist = max( 8.2f, ( RING_RADIUS + 1.9f ) / fTanH );
    // The game row only needs the front case and its neighbours in frame
    g_fCamDistLib = max( 6.5f, 2.6f / fTanH );
    g_fCamZ = g_fCamDist;
}


//-------------------------------------------------------------------------------------
// Name: InitD3D()
// Desc: Creates the device at the size InitScreen() chose.
//-------------------------------------------------------------------------------------
HRESULT InitD3D()
{
    Direct3D* pD3D = Direct3DCreate9( D3D_SDK_VERSION );
    if( !pD3D )
        return E_FAIL;

    D3DPRESENT_PARAMETERS d3dpp;
    ZeroMemory( &d3dpp, sizeof(d3dpp) );
    d3dpp.BackBufferWidth        = g_Screen.Width;
    d3dpp.BackBufferHeight       = g_Screen.Height;
    d3dpp.BackBufferFormat       = D3DFMT_X8R8G8B8;
    d3dpp.BackBufferCount        = 1;
    d3dpp.EnableAutoDepthStencil = TRUE;
    d3dpp.AutoDepthStencilFormat = D3DFMT_D24S8;
    d3dpp.SwapEffect             = D3DSWAPEFFECT_DISCARD;
    d3dpp.PresentationInterval   = D3DPRESENT_INTERVAL_ONE;

    if( FAILED( pD3D->CreateDevice( 0, D3DDEVTYPE_HAL, NULL,
                                    D3DCREATE_HARDWARE_VERTEXPROCESSING,
                                    &d3dpp, &g_pd3dDevice ) ) )
        return E_FAIL;

    return S_OK;
}


//-------------------------------------------------------------------------------------
// Name: InitTime() / UpdateTime()
//-------------------------------------------------------------------------------------
void InitTime()
{
    LARGE_INTEGER qwTicksPerSec;
    QueryPerformanceFrequency( &qwTicksPerSec );
    g_Time.fSecsPerTick = 1.0f / (float)qwTicksPerSec.QuadPart;
    QueryPerformanceCounter( &g_Time.qwLast );
    g_Time.fElapsed = 0.0f;
    g_Time.fOrbTime = 0.0f;
}

// Returns this frame's step, clamped like the prototype so a stall can't jump the animation
float UpdateTime()
{
    LARGE_INTEGER qwNow;
    QueryPerformanceCounter( &qwNow );
    float fDelta = g_Time.fSecsPerTick * (float)( qwNow.QuadPart - g_Time.qwLast.QuadPart );
    g_Time.qwLast = qwNow;

    float dt = min( fDelta, 0.05f );
    g_Time.fElapsed += dt;
    g_Time.fOrbTime += dt;

    // Wrap well before float precision runs out (about 4.6 hours); the noise
    // slides once at the wrap, which is invisible in practice
    if( g_Time.fElapsed > 16384.0f ) { g_Time.fElapsed -= 16384.0f; g_Time.fOrbTime -= 16384.0f; }
    return dt;
}


//-------------------------------------------------------------------------------------
// Name: PollInput()
// Desc: Reads every controller slot (the pad may not sit in slot 0 after a
//       relaunch). Pressed = newly down this frame; Held = down now; StickX = the
//       left stick furthest from centre, -1..1.
//-------------------------------------------------------------------------------------
struct InputState
{
    WORD  wPressed;
    WORD  wHeld;
    float fStickX;
    float fStickY;   // up is positive
};

void PollInput( InputState* pIn )
{
    ZeroMemory( pIn, sizeof(*pIn) );

    for( DWORD i = 0; i < XUSER_MAX_COUNT; i++ )
    {
        XINPUT_STATE State;
        ZeroMemory( &State, sizeof(State) );
        if( XInputGetState( i, &State ) != ERROR_SUCCESS )
        {
            g_wPrevButtons[i] = 0xFFFF;   // unplugged: re-arm only after we see buttons released
            g_wArmed[i] = 0;
            continue;
        }

        WORD wButtons = State.Gamepad.wButtons;
        if( State.Gamepad.bLeftTrigger > XINPUT_GAMEPAD_TRIGGER_THRESHOLD )
            wButtons |= PAD_LEFT_TRIGGER;   // LT pulled counts as a button
        pIn->wPressed |= wButtons & ~g_wPrevButtons[i];
        // A held button only counts once it has been seen released
        g_wArmed[i] |= (WORD)~wButtons;
        pIn->wHeld  |= wButtons & g_wArmed[i];
        g_wPrevButtons[i] = wButtons;

        float x = State.Gamepad.sThumbLX / 32767.0f;
        if( fabsf( x ) > fabsf( pIn->fStickX ) )
            pIn->fStickX = x;
        float y = State.Gamepad.sThumbLY / 32767.0f;
        if( fabsf( y ) > fabsf( pIn->fStickY ) )
            pIn->fStickY = y;
    }
}


//-------------------------------------------------------------------------------------
// Name: UpdateMenu()
// Desc: Sends this frame's input to whichever view is open and plays its sounds.
//       Left / right (D-pad or stick past half) moves at once and repeats every
//       200ms while held. On the main menu, A pulses the orb and opens the item.
//-------------------------------------------------------------------------------------
float g_fRepeat  = 0.0f;   // seconds until a held direction moves again
float g_fRepeatY = 0.0f;

int MenuDirection( const InputState& in )
{
    BOOL bLeft  = ( in.wHeld & XINPUT_GAMEPAD_DPAD_LEFT )  || in.fStickX < -0.5f;
    BOOL bRight = ( in.wHeld & XINPUT_GAMEPAD_DPAD_RIGHT ) || in.fStickX >  0.5f;
    if( bLeft == bRight )
        return 0;
    return bLeft ? -1 : 1;
}

// Down is +1 (the next row)
int MenuDirectionY( const InputState& in )
{
    BOOL bUp   = ( in.wHeld & XINPUT_GAMEPAD_DPAD_UP )   || in.fStickY >  0.5f;
    BOOL bDown = ( in.wHeld & XINPUT_GAMEPAD_DPAD_DOWN ) || in.fStickY < -0.5f;
    if( bUp == bDown )
        return 0;
    return bUp ? -1 : 1;
}

// A held direction moves at once, then every 200ms (the prototype's pollPad)
int Repeat( int nDir, float* pfRepeat, float dt )
{
    if( nDir == 0 )
    {
        *pfRepeat = 0.0f;
        return 0;
    }
    *pfRepeat -= dt;
    if( *pfRepeat > 0.0f )
        return 0;
    *pfRepeat = 0.2f;
    return nDir;
}

static float Damp( float x, float y, float fLambda, float dt )
{
    return x + ( y - x ) * ( 1.0f - expf( -fLambda * dt ) );
}

// While the library updates (a scan, then any box art decoding for the first time) a
// LIBRARY card shows the progress and the controller waits, so nothing opens half loaded
// or hitches. Box art already in the cache, and drawn covers, never hold it up.
BOOL g_bBusyCard = FALSE;

static BOOL LibraryBusy()
{
    return Scan_IsRunning() || Library_CoversRemaining( NULL ) > 0;
}

static void ShowBusyCard()
{
    static const char* c_szScan[]   = { "SCANNING", "FOUND" };
    static const char* c_szCovers[] = { "LOADING", "COVERS" };
    char szA[32], szB[32];
    const char* szValues[2] = { szA, szB };
    if( Scan_IsRunning() )
    {
        strcpy_s( szA, "YOUR DRIVES" );
        sprintf_s( szB, "%d SO FAR", Scan_FoundSoFar() );
        Hud_SetDetails( 2, c_szScan, szValues, NULL, 0, HUD_CARD_LIBRARY );
    }
    else
    {
        int nTotal, nLeft = Library_CoversRemaining( &nTotal );
        strcpy_s( szA, "BOX ART" );
        sprintf_s( szB, "%d / %d", nTotal - nLeft, nTotal );
        Hud_SetDetails( 2, c_szCovers, szValues, NULL, 0, HUD_CARD_LIBRARY );
    }
    g_bBusyCard = TRUE;
}

void UpdateMenu( const InputState& in, float dt )
{
    // While the library updates, the buttons do nothing
    BOOL bNoInput = LibraryBusy();
    int nMove  = Repeat( bNoInput ? 0 : MenuDirection( in ), &g_fRepeat, dt );
    int nMoveY = Repeat( bNoInput ? 0 : MenuDirectionY( in ), &g_fRepeatY, dt );
    WORD wPressed = bNoInput ? 0 : in.wPressed;

    // Sounds: move on any step (up / down only where there's a list), select on A
    // (and Y, which opens details), back on B
    BOOL bList = Files_IsOpen() || Settings_IsOpen();
    if( wPressed & XINPUT_GAMEPAD_B )
        Sound_Play( SOUND_BACK );
    else if( wPressed & ( XINPUT_GAMEPAD_A | XINPUT_GAMEPAD_Y ) )
        Sound_Play( SOUND_SELECT );
    else if( nMove || ( nMoveY && bList ) ||
             ( wPressed & ( XINPUT_GAMEPAD_X | XINPUT_GAMEPAD_RIGHT_SHOULDER ) ) ||
             ( Files_IsOpen() && ( wPressed & ( XINPUT_GAMEPAD_LEFT_SHOULDER | PAD_LEFT_TRIGGER ) ) ) )
        Sound_Play( SOUND_MOVE );

    // A finished scan refills the shelves
    if( Scan_Poll() )
        Library_OnLibraryChanged();

    if( Files_IsOpen() || Settings_IsOpen() )
    {
        Ring_Update( dt, 0 );
        Library_Update( dt, 0, 0, &g_fPulse );
        Files_Update( dt, nMove, nMoveY, wPressed );
        Settings_Update( dt, nMove, nMoveY, wPressed );
    }
    else if( Library_IsOpen() )
    {
        Ring_Update( dt, 0 );
        Library_Update( dt, nMove, wPressed, &g_fPulse );
    }
    else
    {
        Ring_Update( dt, g_fOpenIn < 0.0f ? nMove : 0 );
        Library_Update( dt, 0, 0, &g_fPulse );

        if( ( wPressed & XINPUT_GAMEPAD_A ) && g_fOpenIn < 0.0f )
        {
            // The orb pulses, then the view opens
            g_fPulse = 1.0f;
            g_fOpenIn = 0.25f;   // the prototype opens the view 250ms after select
        }
        if( g_fOpenIn >= 0.0f )
        {
            g_fOpenIn -= dt;
            if( g_fOpenIn < 0.0f )
            {
                const char* szItem = Ring_GetLabel( Ring_GetIndex() );
                if( strcmp( szItem, "FILES" ) == 0 )
                    Files_Open();
                else if( strcmp( szItem, "SETTINGS" ) == 0 )
                    Settings_Open();
                else
                    Library_Open( szItem );
            }
        }
    }

    // The LIBRARY card (set last, so a view updating its own HUD this frame can't hide it)
    if( LibraryBusy() )
        ShowBusyCard();
    else if( g_bBusyCard )
    {
        Hud_SetDetails( 0, NULL, NULL, NULL, 0 );
        g_bBusyCard = FALSE;
    }

    // Shelf open: ring tucks away, orb floats up, camera comes in
    BOOL bLib = Library_IsOpen();
    g_fRingScale = Damp( g_fRingScale, bLib ? 0.001f : 1.0f, 7.0f, dt );
    g_fOrbY      = Damp( g_fOrbY, bLib ? 0.6f : 0.0f, 5.0f, dt );
    g_fCamZ      = Damp( g_fCamZ, bLib ? g_fCamDistLib : g_fCamDist, 4.0f, dt );
    Ring_SetScale( g_fRingScale );
}


//-------------------------------------------------------------------------------------
// Name: Render()
// Desc: The prototype's camera: slow drift around (0.3 sideways, 1.2 up, camDist
//       back), always looking at the orb. Right-handed like three.js.
//-------------------------------------------------------------------------------------
void Render()
{
    float et = g_Time.fElapsed;
    XMVECTOR vEye    = XMVectorSet( sinf( et * 0.1f ) * 0.3f, 1.2f + sinf( et * 0.13f ) * 0.15f, g_fCamZ, 1.0f );
    XMVECTOR vTarget = XMVectorSet( 0.0f, 0.0f, 0.0f, 1.0f );
    XMVECTOR vUp     = XMVectorSet( 0.0f, 1.0f, 0.0f, 0.0f );

    XMMATRIX matView = XMMatrixLookAtRH( vEye, vTarget, vUp );
    XMMATRIX matProj = XMMatrixPerspectiveFovRH( XMConvertToRadians( CAMERA_FOV ),
                                                 g_Screen.DisplayAspect, 0.1f, 100.0f );
    XMMATRIX matViewProj = matView * matProj;

    // Orb centre's view depth: ring pieces behind it draw before the shell, the rest after
    float fOrbDepth = -XMVectorGetZ( XMVector3Transform( vTarget, matView ) );

    // Shelf covers coming into view get drawn first, in their own render target
    Library_PrepareCovers( g_pd3dDevice );

    // three.js order: background, opaque (nucleus), then transparent far to near
    Post_BeginScene( g_pd3dDevice );
    Backdrop_RenderBackground( g_pd3dDevice );
    Orb_RenderNucleus( g_pd3dDevice, matViewProj, g_Time.fOrbTime, g_fPulse, g_fOrbY );
    Ring_Render( g_pd3dDevice, matView, matProj, vEye, RING_BACK, fOrbDepth );
    Backdrop_RenderStars( g_pd3dDevice, matViewProj );
    Orb_RenderShell( g_pd3dDevice, matViewProj, vEye, g_Time.fOrbTime, g_fPulse, g_fOrbY );
    Ring_Render( g_pd3dDevice, matView, matProj, vEye, RING_FRONT_TRACK, fOrbDepth );
    Post_EndFrame( g_pd3dDevice );
    Post_SetSceneDepth( g_pd3dDevice, TRUE );
    Ring_Render( g_pd3dDevice, matView, matProj, vEye, RING_BACK_PANELS, fOrbDepth );
    Post_SetSceneDepth( g_pd3dDevice, FALSE );
    Ring_Render( g_pd3dDevice, matView, matProj, vEye, RING_FRONT_PANELS, fOrbDepth );
    Library_Render( g_pd3dDevice, matView, matProj );
    Files_Render( g_pd3dDevice );
    Settings_Render( g_pd3dDevice );

    Hud_Render( g_pd3dDevice );
    g_pd3dDevice->Present( NULL, NULL, NULL, NULL );
}


//-------------------------------------------------------------------------------------
// Name: main()
// Desc: The application's entry point
//-------------------------------------------------------------------------------------
void __cdecl main()
{
    for( DWORD i = 0; i < XUSER_MAX_COUNT; i++ )
    {
        g_wPrevButtons[i] = 0xFFFF;
        g_wArmed[i] = 0;
    }

    InitScreen();

    // Never return from main or spin on a dead device: on failure, go back to
    // the default app instead of leaving a black screen that needs a power cycle.
    if( FAILED( InitD3D() ) )
        ExitToDefaultApp();
    if( FAILED( Post_Init( g_pd3dDevice ) ) )
        ExitToDefaultApp();
    if( FAILED( Backdrop_Init( g_pd3dDevice ) ) )
        ExitToDefaultApp();
    if( FAILED( Orb_Init( g_pd3dDevice ) ) )
        ExitToDefaultApp();
    // Ring and HUD art comes from game:\Media; a missing file leaves that piece out
    if( FAILED( Ring_Init( g_pd3dDevice ) ) )
        ExitToDefaultApp();
    if( FAILED( Hud_Init( g_pd3dDevice ) ) )
        ExitToDefaultApp();
    if( FAILED( Library_Init( g_pd3dDevice ) ) )
        ExitToDefaultApp();
    if( FAILED( Files_Init( g_pd3dDevice ) ) )   // also links the drive names
        ExitToDefaultApp();
    if( FAILED( Settings_Init( g_pd3dDevice ) ) )
        ExitToDefaultApp();
    Profile_Init( g_pd3dDevice );                  // the top bar's profile badge
    Sound_Init();                                  // the hum fades in while the rest loads
    Scan_Init();                                   // saved library, or a first scan

    InitTime();

    for(;;)
    {
        float dt = UpdateTime();

        Profile_Update( g_pd3dDevice );

        InputState in;
        PollInput( &in );
        // B goes back inside a view (the view handles it) and does nothing on the main
        // menu: Orbit is the default app now, so leaving to it would just restart Orbit
        UpdateMenu( in, dt );
        Hud_Update( dt );
        g_fPulse = max( 0.0f, g_fPulse - dt * 2.5f );

        Render();
    }
}
