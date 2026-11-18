// Orbit, an Xbox 360 homebrew launcher. Copyright (c) 2026 Noah Colbourne.
// Licensed under the GNU General Public License v3.0 or later; see LICENSE.

// Library.h : the shelf views (XBOX, XBOX 360, ARCADE, APPS), the prototype's Xbox
// library. The menu ring tucks away, the orb floats up and the shelf's items (from the
// scan, Scan.h) fan out as 3D cases in a light cover-flow.
//
//   left / right  browse        A  launch
//   X             flip to the back cover (XBOX and XBOX 360; arcade and apps have none)
//   Y             details card: title, version, size, delete
//   B             back to the menu (closes the details card first)

#pragma once

HRESULT Library_Init( D3DDevice* pDevice );

// Opens the shelf for a ring item; FALSE if that item has no shelf
BOOL    Library_Open( const char* szRingLabel );
BOOL    Library_IsOpen();

// Call when a scan has replaced the library (Scan_Poll returned TRUE)
void    Library_OnLibraryChanged();

// Input while open. nMove = -1, 0, +1 (already repeat-limited); wPressed = newly
// pressed buttons. Sets *pfPulse to 1 on launch. Closes itself on B.
void    Library_Update( float dt, int nMove, WORD wPressed, float* pfPulse );

// Items whose box art is still to load (images decoding for the first time, or a rescan
// not yet matched up); *pnTotal = all items
int     Library_CoversRemaining( int* pnTotal );

// Loads box art still missing (cache reads while the frame has time, at most one decode,
// and now and then one that ran short of memory before), lets go of drawn covers far
// from the view and makes the near ones. Call before the frame's scene starts (it uses
// its own render target).
void    Library_PrepareCovers( D3DDevice* pDevice );

// Cases, onto the back buffer after Post_EndFrame (no depth), far to near
void    Library_Render( D3DDevice* pDevice, const XMMATRIX& matView, const XMMATRIX& matProj );
