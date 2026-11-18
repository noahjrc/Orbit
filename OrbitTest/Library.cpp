// Orbit, an Xbox 360 homebrew launcher. Copyright (c) 2026 Noah Colbourne.
// Licensed under the GNU General Public License v3.0 or later; see LICENSE.

// Library.cpp : see Library.h. The prototype's library, ported:
//
//   library group : position (0, 0.3, 2.2), scale eases 0.001 <-> 1 (damp 7)
//   cases         : BoxGeometry(0.95, 1.34, 0.12), faces +x edge tone, -x spine,
//                   +y edge tone, -y plastic, +z cover, -z plastic (here: back cover)
//   layout        : d = i - index; front case at z 0.55, scale 1.18, swaying;
//                   neighbours at x = sign(d) * (0.95 + (|d| - 1) * 0.5),
//                   z = -0.15 - |d| * 0.08, scale 0.92, all turned +0.95 so the spine
//                   shows on both sides (the prototype mirrors the right side);
//                   all damp 9, hidden past |d| = 4
//                   (ARCADE and APPS show thin tiles facing the camera instead)
//   look          : no outline (the prototype draws rim lines on each box); black
//                   plastic for original Xbox, green for 360, orange for Arcade, covers
//                   inside a lip with a sheen
//   cover level   : 1 front / 0.55 others (the side cases are dimmer)
//
// The items come from the scan (Scan.h). Box art comes from the Covers folder next to
// Orbit.xex (game:\Covers\<title ID>.jpg or .png). A full box scan (back | spine | front side by side, XboxUnity's
// 900 x 600 layout) wraps the whole case and gives it a back cover; a portrait image
// is just the front. Decoding an image is slow, so the first load saves the result in
// a Cache folder beside it and later boots copy that in instead. ARCADE tiles are
// square and show the icon from the game's package (Covers\Icons\<title ID>.png, saved
// by the scan) inside the orange lip, not the Covers folder's art. Without art, a cover
// is made here on the console: a baked template (one of six hues, picked from the
// title) with the title written on it, rendered to a 256 x 352 texture. Box art (DXT1,
// small) stays loaded for every item; drawn covers (uncompressed, ~360 KB each) are only
// kept near the view and made a few cases ahead of it, so a long shelf doesn't fill memory.
// A rescan keeps the box art of items still there instead of loading it all again.
//
// Unlike the prototype the cases draw after bloom (as the front menu panels do), so
// the orb's glow never washes over them.
//

#include "stdafx.h"
#include <ctype.h>
#include <d3dx9tex.h>
#include <xgraphics.h>
#include "Screen.h"
#include "Assets.h"
#include "Post.h"
#include "Hud.h"
#include "Drives.h"
#include "Scan.h"
#include "Library.h"

#include "Shaders\LabelVS.h"
#include "Shaders\CasePS.h"

// System exports the XDK headers don't declare (xapilib.lib); signatures as Freestyle Dash uses them
extern "C"
{
    // Mounts a content package as the drive szRootName: (0xFE = no particular user)
    HRESULT XamContentOpenFile( DWORD dwUserIndex, const char* szRootName, const char* szPath, DWORD dwFlags,
                                DWORD dwUnk1, DWORD dwUnk2, DWORD* pdwUnk3 );
    HRESULT XamContentClose( const char* szRootLink, DWORD* pdwUnk );
}

static const char* c_szShelfNames[NUM_SHELVES] = { "XBOX", "XBOX 360", "ARCADE", "APPS" };
static const char* c_szShelfKinds[NUM_SHELVES] = { "ORIGINAL XBOX", "XBOX 360", "ARCADE", "APP" };
static const BOOL  c_bShelfBacks[NUM_SHELVES]  = { TRUE, TRUE, FALSE, FALSE };   // drawn backs: only boxed games
// Covers folders, searched in order: only Orbit's own folder (on whatever drive it runs
// from). Not Usb0:\Orbit\Covers, where versions before 0.9.1 looked: touching a USB hard
// drive at boot wakes it, which held the first frame back about 14 seconds.
static const char* c_szCoverDirs[] = { "game:\\Covers\\" };

// Where each face sits in a box scan (fractions of its width): back | spine | front
static const float WRAP_SPINE_L = 425.0f / 900.0f, WRAP_SPINE_R = 475.0f / 900.0f;

// Case plastic (sRGB): original Xbox cases are black, 360 (and Arcade) cases the
// translucent green; apps have no box and keep the purple of their drawn cover.
// Edges are the lit sides, plastic the underside; the lip rims the cover insert.
struct CaseLook { BYTE Edge[3], Plastic[3], Lip[3]; };
static const CaseLook c_Looks[NUM_SHELVES] =
{
    { { 0x26, 0x26, 0x28 }, { 0x12, 0x12, 0x13 }, { 0x1c, 0x1c, 0x1e } },   // XBOX: black
    { { 0x4c, 0xa8, 0x52 }, { 0x2a, 0x6e, 0x34 }, { 0x6c, 0xc4, 0x6e } },   // XBOX 360: green
    { { 0xf0, 0x8a, 0x24 }, { 0xa8, 0x55, 0x10 }, { 0xff, 0xa2, 0x45 } },   // ARCADE: the Arcade banner's orange
    { { 0, 0, 0 },          { 0, 0, 0 },          { 0, 0, 0 } },            // APPS: template colours
};

// Boxed shelves show cases turned to their spines; ARCADE and APPS (no boxes) show
// thin tiles facing the camera, fronts only, and never their backs
static const BOOL  c_bShelfBoxed[NUM_SHELVES] = { TRUE, TRUE, FALSE, FALSE };
static const float TILE_DEPTH = 0.25f;   // of a case's depth
// ARCADE tiles are square, the shape of the package icons they show
static const BOOL  c_bShelfSquare[NUM_SHELVES] = { FALSE, FALSE, TRUE, FALSE };
static const float LIP_U = 0.025f, LIP_V = 0.018f;   // about 3mm of a 135 x 190mm case
static const float SHEEN = 0.05f;

static const int   MAX_SHELF = MAX_SCAN_ITEMS;   // a shelf can hold the whole library
static const int   SHOWN_EACH_SIDE = 4;          // cases drawn each side of the front one
static const int   NUM_TEMPLATES = 6;
static const int   TEMPLATE_HUES[NUM_TEMPLATES] = { 265, 300, 235, 320, 280, 250 };
static const UINT  COVER_W = 256, COVER_H = 352;

static const float CASE_W = 0.95f, CASE_H = 1.34f, CASE_D = 0.12f;

//-------------------------------------------------------------------------------------
// State
//-------------------------------------------------------------------------------------
struct CaseState
{
    float x, y, z, fRotY, fScale;
    float fLevel;    // cover colour level
};

static BOOL      s_bOpen     = FALSE;
static int       s_nShelf    = SHELF_XBOX;
static int       s_nIndex    = 0;
static float     s_fLibScale = 0.001f;
static float     s_fTime     = 0.0f;
static BOOL      s_bFlipped  = FALSE;   // front case turned round to its back cover
static BOOL      s_bDetails  = FALSE;
static BOOL      s_bConfirm  = FALSE;   // delete pressed once, waiting for the second press
static BOOL      s_bDeleting = FALSE;   // our delete job is running
static CaseState s_Cases[MAX_SHELF];

static D3DTexture* s_pCoverTpl[NUM_TEMPLATES];
static D3DTexture* s_pBackTpl[NUM_TEMPLATES];
static D3DTexture* s_pCover[NUM_SHELVES][MAX_SHELF];   // drawn covers (no art)
static D3DTexture* s_pBack[NUM_SHELVES][MAX_SHELF];
static D3DTexture* s_pArt[NUM_SHELVES][MAX_SHELF];     // box art from the covers folder
static BYTE        s_Art[NUM_SHELVES][MAX_SHELF];      // ART_* below
// ART_DECODE: its image isn't in the cache yet and waits to be decoded; ART_ICON: an
// ARCADE package's icon
enum { ART_UNTRIED, ART_DECODE, ART_NONE, ART_FRONT, ART_WRAP, ART_ICON };
struct ArtStamp { DWORD dwSize, dwTimeLo, dwTimeHi; };   // an image file's size and time
static DWORD       s_dwArtKey[NUM_SHELVES][MAX_SHELF];  // ArtKey of the art's title ID
static ArtStamp    s_ArtStamp[NUM_SHELVES][MAX_SHELF];  // its image's stamp then
static BOOL        s_bHudDirty = FALSE;                // art arrived for the front case
static BYTE        s_Failed[NUM_SHELVES][MAX_SHELF];   // FAILED_* below: drawn covers that couldn't be made
enum { FAILED_FRONT = 1, FAILED_BACK = 2 };            // (out of memory): left plastic, not retried
static BYTE        s_ArtRetry[NUM_SHELVES][MAX_SHELF]; // decode ran out of memory: retried later
static D3DSurface* s_pCoverRT = NULL;
static float       s_vPlastic[NUM_TEMPLATES][4];   // linear
static float       s_vEdge[NUM_TEMPLATES][4];

static D3DVertexShader*      s_pVS   = NULL;
static D3DPixelShader*       s_pPS   = NULL;
static D3DVertexDeclaration* s_pDecl = NULL;

struct CaseVertex
{
    float Pos[3];
    float UV[2];
};
enum { FACE_PX, FACE_NX, FACE_PY, FACE_NY, FACE_PZ, FACE_NZ, NUM_FACES };
static CaseVertex s_Faces[NUM_FACES][4];   // triangle strips: TL, BL, TR, BR seen from outside


//-------------------------------------------------------------------------------------
// Helpers
//-------------------------------------------------------------------------------------
static float SRGBToLinear( float f )
{
    return ( f <= 0.04045f ) ? f / 12.92f : powf( ( f + 0.055f ) / 1.055f, 2.4f );
}

// CSS hsl() to linear rgb, as three.js Color.setStyle does
static void HslToLinear( float h, float s, float l, float* pOut )
{
    h = fmodf( h, 360.0f ) / 360.0f;
    float q = ( l < 0.5f ) ? l * ( 1.0f + s ) : l + s - l * s;
    float p = 2.0f * l - q;
    for( int k = 0; k < 3; k++ )
    {
        float t = h + ( 1.0f - k ) / 3.0f;   // r: h + 1/3, g: h, b: h - 1/3
        if( t < 0.0f ) t += 1.0f;
        if( t > 1.0f ) t -= 1.0f;
        float c;
        if( t < 1.0f / 6.0f )      c = p + ( q - p ) * 6.0f * t;
        else if( t < 0.5f )        c = q;
        else if( t < 2.0f / 3.0f ) c = p + ( q - p ) * ( 2.0f / 3.0f - t ) * 6.0f;
        else                       c = p;
        pOut[k] = SRGBToLinear( c );
    }
    pOut[3] = 1.0f;
}

static float Damp( float x, float y, float fLambda, float dt )
{
    return x + ( y - x ) * ( 1.0f - expf( -fLambda * dt ) );
}

static int Sign( int n ) { return ( n > 0 ) - ( n < 0 ); }

static void UpperCopy( char* szDst, size_t nSize, const char* szSrc )
{
    size_t i = 0;
    for( ; szSrc[i] && i + 1 < nSize; i++ )
        szDst[i] = (char)toupper( (unsigned char)szSrc[i] );
    szDst[i] = 0;
}

static void FormatSize( ULONGLONG q, char* sz, size_t n )
{
    if( q < 1024 * 1024 )                   sprintf_s( sz, n, "%u KB", (UINT)( q / 1024 ) );
    else if( q < 1024ULL * 1024 * 1024 )    sprintf_s( sz, n, "%.1f MB", q / ( 1024.0 * 1024.0 ) );
    else                                    sprintf_s( sz, n, "%.2f GB", q / ( 1024.0 * 1024.0 * 1024.0 ) );
}

// Which of the six template hues a title gets
static int TemplateFor( const char* szTitle )
{
    DWORD h = 5381;
    for( const char* p = szTitle; *p; p++ )
        h = h * 33 + (unsigned char)toupper( (unsigned char)*p );
    return (int)( h % NUM_TEMPLATES );
}

static int ItemCount() { return min( Scan_ItemCount( s_nShelf ), MAX_SHELF ); }

// One face: centre c, right r and up u as seen from outside (r x u = outward normal)
static void SetFace( CaseVertex* v, XMFLOAT3 c, XMFLOAT3 r, XMFLOAT3 u )
{
    const float sr[4] = { -1.0f, -1.0f, 1.0f, 1.0f };
    const float su[4] = {  1.0f, -1.0f, 1.0f, -1.0f };
    for( int i = 0; i < 4; i++ )
    {
        v[i].Pos[0] = c.x + r.x * sr[i] + u.x * su[i];
        v[i].Pos[1] = c.y + r.y * sr[i] + u.y * su[i];
        v[i].Pos[2] = c.z + r.z * sr[i] + u.z * su[i];
        v[i].UV[0] = ( sr[i] + 1.0f ) * 0.5f;
        v[i].UV[1] = ( 1.0f - su[i] ) * 0.5f;
    }
}


//-------------------------------------------------------------------------------------
// Name: Library_Init()
//-------------------------------------------------------------------------------------
HRESULT Library_Init( D3DDevice* pDevice )
{
    if( FAILED( pDevice->CreateVertexShader( g_pLabelVS, &s_pVS ) ) )  return E_FAIL;
    if( FAILED( pDevice->CreatePixelShader( g_pCasePS, &s_pPS ) ) )    return E_FAIL;

    D3DVERTEXELEMENT9 Elements[] =
    {
        { 0,  0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0 },
        { 0, 12, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0 },
        D3DDECL_END()
    };
    if( FAILED( pDevice->CreateVertexDeclaration( Elements, &s_pDecl ) ) )
        return E_FAIL;

    // Box faces, with three.js BoxGeometry's uv directions
    const float w = CASE_W * 0.5f, h = CASE_H * 0.5f, d = CASE_D * 0.5f;
    SetFace( s_Faces[FACE_PZ], XMFLOAT3( 0, 0,  d ), XMFLOAT3(  w, 0, 0 ), XMFLOAT3( 0, h, 0 ) );
    SetFace( s_Faces[FACE_NZ], XMFLOAT3( 0, 0, -d ), XMFLOAT3( -w, 0, 0 ), XMFLOAT3( 0, h, 0 ) );
    SetFace( s_Faces[FACE_NX], XMFLOAT3( -w, 0, 0 ), XMFLOAT3( 0, 0,  d ), XMFLOAT3( 0, h, 0 ) );
    SetFace( s_Faces[FACE_PX], XMFLOAT3(  w, 0, 0 ), XMFLOAT3( 0, 0, -d ), XMFLOAT3( 0, h, 0 ) );
    SetFace( s_Faces[FACE_PY], XMFLOAT3( 0,  h, 0 ), XMFLOAT3(  w, 0, 0 ), XMFLOAT3( 0, 0, -d ) );
    SetFace( s_Faces[FACE_NY], XMFLOAT3( 0, -h, 0 ), XMFLOAT3(  w, 0, 0 ), XMFLOAT3( 0, 0,  d ) );

    for( int i = 0; i < NUM_TEMPLATES; i++ )
    {
        char szName[16];
        sprintf_s( szName, "cover_tpl%d", i ); Assets_LoadTexture( pDevice, szName, &s_pCoverTpl[i] );
        sprintf_s( szName, "back_tpl%d", i );  Assets_LoadTexture( pDevice, szName, &s_pBackTpl[i] );
        HslToLinear( (float)TEMPLATE_HUES[i], 0.55f, 0.10f, s_vPlastic[i] );
        HslToLinear( (float)TEMPLATE_HUES[i], 0.60f, 0.20f, s_vEdge[i] );
    }

    // Make the covers folder next to Orbit.xex, ready for box art (fails harmlessly if
    // it's already there)
    CreateDirectoryA( "game:\\Covers", NULL );

    // Covers are drawn into this and resolved into their own textures. EDRAM base 0,
    // like the post targets: it's only used before the frame's scene starts.
    D3DSURFACE_PARAMETERS Params;
    ZeroMemory( &Params, sizeof(Params) );
    if( FAILED( pDevice->CreateRenderTarget( COVER_W, COVER_H, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &s_pCoverRT, &Params ) ) )
        s_pCoverRT = NULL;
    return S_OK;
}


//-------------------------------------------------------------------------------------
// Covers
//-------------------------------------------------------------------------------------
// Words of sz in lines no wider than fMaxW; returns the line count (up to nMaxLines,
// or nMaxLines + 1 when it didn't fit)
static int WrapTitle( const char* sz, float fPx, float fMaxW, char szLines[][64], int nMaxLines )
{
    int nLines = 0;
    char szLine[64] = "";
    const char* p = sz;
    while( *p )
    {
        while( *p == ' ' ) p++;
        if( !*p ) break;
        char szWord[64];
        int n = 0;
        while( p[n] && p[n] != ' ' && n < 63 ) { szWord[n] = p[n]; n++; }
        szWord[n] = 0;
        p += n;

        char szTry[128];
        sprintf_s( szTry, szLine[0] ? "%s %s" : "%s%s", szLine, szWord );
        if( szLine[0] && Hud_TextWidth( HUD_FONT_TITLE, szTry, fPx, fPx * 0.02f ) > fMaxW )
        {
            if( nLines >= nMaxLines ) return nMaxLines + 1;
            strcpy_s( szLines[nLines++], 64, szLine );
            strcpy_s( szLine, szWord );
        }
        else
            strncpy_s( szLine, szTry, _TRUNCATE );
    }
    if( szLine[0] )
    {
        if( nLines >= nMaxLines ) return nMaxLines + 1;
        strcpy_s( szLines[nLines++], 64, szLine );
    }
    return nLines;
}

static const HudColor C_TITLE = { 0xfb / 255.0f, 0xef / 255.0f, 0xff / 255.0f, 1.0f };
static const HudColor C_GLOW  = { 0xcc / 255.0f, 0x33 / 255.0f, 0xff / 255.0f, 1.0f };
static const HudColor C_SUB   = { 0xd9 / 255.0f, 0xb8 / 255.0f, 0xff / 255.0f, 1.0f };
static const HudColor C_NONE  = { 0.0f, 0.0f, 0.0f, 0.0f };

static D3DTexture* MakeCover( D3DDevice* pDevice, const ScanItem& it, BOOL bBack )
{
    // Square shelves get a square cover: the template's top (its ORBIT bar) is cut off
    const UINT  nH   = c_bShelfSquare[it.nShelf] ? COVER_W : COVER_H;
    const float fTop = (float)nH - COVER_H;
    D3DTexture* pTex = NULL;
    if( !s_pCoverRT || FAILED( pDevice->CreateTexture( COVER_W, nH, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &pTex, NULL ) ) )
        return NULL;

    pDevice->SetRenderTarget( 0, s_pCoverRT );
    pDevice->SetDepthStencilSurface( NULL );
    D3DVIEWPORT9 vp = { 0, 0, COVER_W, nH, 0.0f, 1.0f };
    pDevice->SetViewport( &vp );
    pDevice->Clear( 0, NULL, D3DCLEAR_TARGET, D3DCOLOR_XRGB( 0, 0, 0 ), 1.0f, 0 );

    int t = TemplateFor( it.szTitle );
    Hud_BeginDrawTarget( pDevice, COVER_W, nH );
    Hud_DrawSprite( pDevice, bBack ? s_pBackTpl[t] : s_pCoverTpl[t], 0.0f, fTop, (float)COVER_W, (float)COVER_H, 1.0f );

    char szTitle[64], szLines[3][64];
    UpperCopy( szTitle, sizeof(szTitle), it.szTitle );
    if( !bBack )
    {
        // Front: centred title up to three lines (the prototype's 54px on 512 wide), shrinking to fit
        float fPx = 27.0f;
        int nLines;
        while( ( nLines = WrapTitle( szTitle, fPx, COVER_W - 35.0f, szLines, 3 ) ) > 3 && fPx > 14.0f )
            fPx -= 2.0f;
        nLines = min( nLines, 3 );
        for( int k = 0; k < nLines; k++ )
        {
            float w = Hud_TextWidth( HUD_FONT_TITLE, szLines[k], fPx, fPx * 0.02f );
            float y = ( fTop < 0.0f ? 28.0f : 65.0f ) + k * fPx * 1.12f + fPx * 0.35f;
            Hud_DrawText( pDevice, HUD_FONT_TITLE, szLines[k], COVER_W * 0.5f - w * 0.5f, y, fPx, fPx * 0.02f, C_TITLE, C_GLOW );
        }
        // Bottom strip: what it is
        const char* szKind = c_szShelfKinds[it.nShelf];
        float w = Hud_TextWidth( HUD_FONT_MONO, szKind, 10.0f, 2.0f );
        Hud_DrawText( pDevice, HUD_FONT_MONO, szKind, COVER_W * 0.5f - w * 0.5f, nH - 9.0f, 10.0f, 2.0f, C_SUB, C_NONE );
    }
    else
    {
        // Back: title left aligned, then version and size
        float fPx = 17.0f;
        int nLines;
        while( ( nLines = WrapTitle( szTitle, fPx, COVER_W - 30.0f, szLines, 2 ) ) > 2 && fPx > 11.0f )
            fPx -= 1.0f;
        nLines = min( nLines, 2 );
        float y = 50.0f;
        for( int k = 0; k < nLines; k++ )
        {
            Hud_DrawText( pDevice, HUD_FONT_TITLE, szLines[k], 15.0f, y + fPx * 0.35f, fPx, fPx * 0.02f, C_TITLE, C_GLOW );
            y += fPx * 1.15f;
        }
        char szSize[32], szSub[96];
        FormatSize( it.qwSize, szSize, sizeof(szSize) );
        sprintf_s( szSub, "VERSION %s \x03 %s", it.szVersion, szSize );
        Hud_DrawText( pDevice, HUD_FONT_MONO, szSub, 15.0f, y + 8.0f, 10.0f, 1.5f, C_SUB, C_NONE );
    }
    Hud_EndDraw( pDevice );

    const D3DRECT Rect = { 0, 0, COVER_W, nH };
    pDevice->Resolve( D3DRESOLVE_RENDERTARGET0, &Rect, pTex, NULL, 0, 0, NULL, 0.0f, 0, NULL );
    return pTex;
}

// Box art cache: Covers\Cache\<title ID>.dxt beside the image, holding the texture
// D3DX made from it so later boots skip the decode. Big-endian, the 360's order:
//
//   "OCV2", image size, image time (low, high), width, height, levels, then each
//   level's DXT1 blocks untiled, row after row
//
// The image's size and time are kept so a replaced image is decoded again. Only the
// levels above the packed mip tail are made (the smallest ones share one tile and
// can't be copied level by level); a case on screen never gets that small.
static const DWORD CACHE_HEADER = 7 * 4;
static const DWORD MAX_CACHE    = 16 * 1024 * 1024;

// Mip levels a cover gets: all of them down to the packed tail (0 = the image is that small)
static UINT ArtLevels( UINT w, UINT h )
{
    return XGGetMipTailBaseLevel( w, h, FALSE );
}

// Nearest power of two to n, from 128 to 1024 (past 1.5x a step it rounds up)
static UINT NearestPow2( UINT n )
{
    UINT p = 128;
    while( p < 1024 && n > p + p / 2 )
        p *= 2;
    return p;
}

// Bytes of one level's untiled DXT1 blocks (8 bytes per 4 x 4 block)
static UINT LevelBytes( UINT w0, UINT h0, UINT lv, UINT* pnPitch )
{
    UINT w = max( 1u, w0 >> lv ), h = max( 1u, h0 >> lv );
    *pnPitch = ( ( w + 3 ) / 4 ) * 8;
    return *pnPitch * ( ( h + 3 ) / 4 );
}

static D3DTexture* LoadCachedArt( D3DDevice* pDevice, const char* szCache, const ArtStamp& Stamp )
{
    HANDLE hFile = CreateFile( szCache, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                               FILE_ATTRIBUTE_NORMAL, NULL );
    if( hFile == INVALID_HANDLE_VALUE )
        return NULL;
    // The header first, so an old or stale file is turned away without reading its pixels
    DWORD dwSize = GetFileSize( hFile, NULL ), dwRead = 0;
    BYTE Head[CACHE_HEADER] = { 0 };
    BOOL bHead = dwSize != INVALID_FILE_SIZE && dwSize > CACHE_HEADER && dwSize <= MAX_CACHE &&
                 ReadFile( hFile, Head, CACHE_HEADER, &dwRead, NULL ) && dwRead == CACHE_HEADER;
    DWORD vHead[3];
    memcpy( vHead, Head + 4, sizeof(vHead) );
    BYTE* pData = NULL;
    if( bHead && memcmp( Head, "OCV2", 4 ) == 0 &&
        vHead[0] == Stamp.dwSize && vHead[1] == Stamp.dwTimeLo && vHead[2] == Stamp.dwTimeHi )
    {
        pData = new BYTE[dwSize];
        memcpy( pData, Head, CACHE_HEADER );
        if( !ReadFile( hFile, pData + CACHE_HEADER, dwSize - CACHE_HEADER, &dwRead, NULL ) || dwRead != dwSize - CACHE_HEADER )
        {
            delete[] pData;
            pData = NULL;
        }
    }
    CloseHandle( hFile );
    if( !pData )
        return NULL;

    // The header's values after the magic (memcpy: no alignment assumed)
    DWORD v[6];
    memcpy( v, pData + 4, sizeof(v) );
    const UINT w0 = v[3], h0 = v[4], nLevels = v[5];
    BOOL bOK = memcmp( pData, "OCV2", 4 ) == 0 &&
               v[0] == Stamp.dwSize && v[1] == Stamp.dwTimeLo && v[2] == Stamp.dwTimeHi &&
               w0 > 0 && h0 > 0 && w0 <= 4096 && h0 <= 4096 && nLevels > 0 && nLevels == ArtLevels( w0, h0 );
    DWORD dwNeed = CACHE_HEADER;
    UINT nPitch;
    for( UINT lv = 0; bOK && lv < nLevels; lv++ )
        dwNeed += LevelBytes( w0, h0, lv, &nPitch );

    D3DTexture* pTex = NULL;
    if( bOK && ( dwNeed != dwSize ||
                 FAILED( pDevice->CreateTexture( w0, h0, nLevels, 0, D3DFMT_DXT1, D3DPOOL_MANAGED, &pTex, NULL ) ) ) )
        bOK = FALSE;

    const BYTE* p = pData + CACHE_HEADER;
    for( UINT lv = 0; bOK && lv < nLevels; lv++ )
    {
        UINT nBytes = LevelBytes( w0, h0, lv, &nPitch );
        D3DLOCKED_RECT Locked;
        if( FAILED( pTex->LockRect( lv, &Locked, NULL, 0 ) ) )
        {
            bOK = FALSE;
            break;
        }
        XGTileTextureLevel( w0, h0, lv, XGGetGpuFormat( D3DFMT_DXT1 ), 0, Locked.pBits, NULL, p, nPitch, NULL );
        pTex->UnlockRect( lv );
        p += nBytes;
    }

    delete[] pData;
    if( !bOK && pTex )
    {
        pTex->Release();
        pTex = NULL;
    }
    return pTex;
}

// Saves a texture D3DX just made into the cache (szDir is the covers folder)
static void SaveCachedArt( const char* szDir, const char* szCache, const ArtStamp& Stamp, D3DTexture* pTex )
{
    D3DSURFACE_DESC Desc;
    pTex->GetLevelDesc( 0, &Desc );
    const UINT w0 = Desc.Width, h0 = Desc.Height, nLevels = pTex->GetLevelCount();
    if( Desc.Format != D3DFMT_DXT1 || nLevels == 0 || nLevels != ArtLevels( w0, h0 ) )
        return;

    DWORD dwSize = CACHE_HEADER;
    UINT nPitch;
    for( UINT lv = 0; lv < nLevels; lv++ )
        dwSize += LevelBytes( w0, h0, lv, &nPitch );
    if( dwSize > MAX_CACHE )
        return;

    BYTE* pData = new BYTE[dwSize];
    const DWORD v[6] = { Stamp.dwSize, Stamp.dwTimeLo, Stamp.dwTimeHi, w0, h0, nLevels };
    memcpy( pData, "OCV2", 4 );
    memcpy( pData + 4, v, sizeof(v) );
    BYTE* p = pData + CACHE_HEADER;
    for( UINT lv = 0; lv < nLevels; lv++ )
    {
        UINT nBytes = LevelBytes( w0, h0, lv, &nPitch );
        D3DLOCKED_RECT Locked;
        if( FAILED( pTex->LockRect( lv, &Locked, NULL, D3DLOCK_READONLY ) ) )
        {
            delete[] pData;
            return;
        }
        XGUntileTextureLevel( w0, h0, lv, XGGetGpuFormat( D3DFMT_DXT1 ), 0, p, nPitch, NULL, Locked.pBits, NULL );
        pTex->UnlockRect( lv );
        p += nBytes;
    }

    // Written under another name and renamed when whole (as the scan saves icons), so a
    // half-written file (the console turned off) is never used
    char szCacheDir[MAX_PATH], szTemp[MAX_PATH];
    sprintf_s( szCacheDir, "%sCache", szDir );
    CreateDirectory( szCacheDir, NULL );
    sprintf_s( szTemp, "%s.tmp", szCache );
    HANDLE hFile = CreateFile( szTemp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL );
    if( hFile != INVALID_HANDLE_VALUE )
    {
        DWORD dwWritten = 0;
        BOOL bOK = WriteFile( hFile, pData, dwSize, &dwWritten, NULL ) && dwWritten == dwSize;
        CloseHandle( hFile );
        DeleteFile( szCache );   // MoveFile won't replace an old cache file
        if( !bOK || !MoveFile( szTemp, szCache ) )
            DeleteFile( szTemp );
    }
    delete[] pData;
}

// Box art for an item from the covers folder, compressed to DXT1 with mips (from the
// cache when it's there). A wide image (a box scan) wraps the case; a tall one is the front.
// ARCADE items only show their package's icon (Covers\Icons, saved by the scan), kept
// uncompressed: DXT1 smears an image that small.
//
// Without bDecode an image that isn't in the cache yet is left as ART_DECODE, for
// PrepareCovers to decode later (one a frame, under the LIBRARY card). An image D3DX
// can't read gets a marker, Cache\<title ID>.<ext>.bad holding its stamp, so it isn't
// tried again every boot; replacing the image gives it a new stamp and another try.
static const char* c_szArtExt[] = { "jpg", "png" };

// FNV-1a of a title ID, case folded: which box art a texture shows, kept across rescans
static DWORD ArtKey( const char* szTitleId )
{
    DWORD h = 2166136261u;
    for( const char* p = szTitleId; *p; p++ )
        h = ( h ^ (BYTE)toupper( (unsigned char)*p ) ) * 16777619u;
    return h;
}

// The first cover image for a title ID (jpg, then png), and its stamp
static BOOL FindArtFile( const char* szTitleId, ArtStamp* pStamp )
{
    for( int d = 0; d < ARRAYSIZE( c_szCoverDirs ); d++ )
        for( int e = 0; e < ARRAYSIZE( c_szArtExt ); e++ )
        {
            char szPath[MAX_PATH];
            sprintf_s( szPath, "%s%s.%s", c_szCoverDirs[d], szTitleId, c_szArtExt[e] );
            WIN32_FILE_ATTRIBUTE_DATA Attr;
            if( GetFileAttributesExA( szPath, GetFileExInfoStandard, &Attr ) )
            {
                if( pStamp )
                {
                    pStamp->dwSize   = Attr.nFileSizeLow;
                    pStamp->dwTimeLo = Attr.ftLastWriteTime.dwLowDateTime;
                    pStamp->dwTimeHi = Attr.ftLastWriteTime.dwHighDateTime;
                }
                return TRUE;
            }
        }
    return FALSE;
}

static BOOL IsMarkedBad( const char* szBad, const ArtStamp& Stamp )
{
    HANDLE h = CreateFile( szBad, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL );
    if( h == INVALID_HANDLE_VALUE )
        return FALSE;
    BYTE Data[16] = { 0 };
    DWORD dwRead = 0;
    BOOL bOK = ReadFile( h, Data, sizeof(Data), &dwRead, NULL ) && dwRead == sizeof(Data);
    CloseHandle( h );
    DWORD v[3];
    memcpy( v, Data + 4, sizeof(v) );
    return bOK && memcmp( Data, "OCVB", 4 ) == 0 &&
           v[0] == Stamp.dwSize && v[1] == Stamp.dwTimeLo && v[2] == Stamp.dwTimeHi;
}

// A half-written marker is the wrong size, so it's never trusted
static void MarkBad( const char* szDir, const char* szBad, const ArtStamp& Stamp )
{
    char szCacheDir[MAX_PATH];
    sprintf_s( szCacheDir, "%sCache", szDir );
    CreateDirectory( szCacheDir, NULL );
    BYTE Data[16];
    const DWORD v[3] = { Stamp.dwSize, Stamp.dwTimeLo, Stamp.dwTimeHi };
    memcpy( Data, "OCVB", 4 );
    memcpy( Data + 4, v, sizeof(v) );
    HANDLE h = CreateFile( szBad, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL );
    if( h != INVALID_HANDLE_VALUE )
    {
        DWORD dwWritten;
        WriteFile( h, Data, sizeof(Data), &dwWritten, NULL );
        CloseHandle( h );
    }
}

static int ShelfCount( int nShelf ) { return min( Scan_ItemCount( nShelf ), MAX_SHELF ); }

// Another item with the same title ID (a game both as a folder and as a package) that
// already has its box art shares the texture instead of loading a second copy
static BOOL ShareArt( int nShelf, int i )
{
    const ScanItem& it = Scan_GetItem( nShelf, i );
    const BOOL bIcon = nShelf == SHELF_ARCADE;
    for( int s = 0; s < NUM_SHELVES; s++ )
    {
        if( ( s == SHELF_ARCADE ) != bIcon )
            continue;
        for( int k = 0; k < ShelfCount( s ); k++ )
        {
            if( ( s == nShelf && k == i ) || !s_pArt[s][k] || _stricmp( Scan_GetItem( s, k ).szTitleId, it.szTitleId ) != 0 )
                continue;
            s_pArt[nShelf][i] = s_pArt[s][k];
            s_pArt[nShelf][i]->AddRef();
            s_Art[nShelf][i]      = s_Art[s][k];
            s_dwArtKey[nShelf][i] = s_dwArtKey[s][k];
            s_ArtStamp[nShelf][i] = s_ArtStamp[s][k];
            return TRUE;
        }
    }
    return FALSE;
}

static const DWORD DECODE_ROOM = 16 * 1024 * 1024;

static BOOL RoomToDecode()
{
    MEMORYSTATUS ms;
    GlobalMemoryStatus( &ms );
    return ms.dwAvailPhys >= DECODE_ROOM;
}

// D3DX's out-of-memory codes aren't documented, so low free memory counts too
static BOOL MemoryShort( HRESULT hr )
{
    return hr == E_OUTOFMEMORY || hr == D3DERR_OUTOFVIDEOMEMORY || !RoomToDecode();
}

static void LoadArt( D3DDevice* pDevice, int nShelf, int i, BOOL bDecode )
{
    s_Art[nShelf][i] = ART_NONE;
    s_ArtRetry[nShelf][i] = FALSE;
    const ScanItem& it = Scan_GetItem( nShelf, i );
    if( !it.szTitleId[0] )
        return;
    if( ShareArt( nShelf, i ) )
        return;
    if( nShelf == SHELF_ARCADE )
    {
        char szIcon[MAX_PATH];
        sprintf_s( szIcon, "game:\\Covers\\Icons\\%s.png", it.szTitleId );
        D3DTexture* pTex = NULL;
        if( SUCCEEDED( D3DXCreateTextureFromFileExA( pDevice, szIcon, D3DX_DEFAULT_NONPOW2, D3DX_DEFAULT_NONPOW2, D3DX_DEFAULT,
                                                     0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, D3DX_DEFAULT, D3DX_DEFAULT, 0, NULL, NULL, &pTex ) ) )
        {
            const ArtStamp NoStamp = { 0, 0, 0 };
            s_pArt[nShelf][i] = pTex;
            s_Art[nShelf][i] = ART_ICON;
            s_dwArtKey[nShelf][i] = ArtKey( it.szTitleId );
            s_ArtStamp[nShelf][i] = NoStamp;
        }
        return;
    }
    for( int d = 0; d < ARRAYSIZE( c_szCoverDirs ); d++ )
        for( int e = 0; e < ARRAYSIZE( c_szArtExt ); e++ )
        {
            char szPath[MAX_PATH];
            sprintf_s( szPath, "%s%s.%s", c_szCoverDirs[d], it.szTitleId, c_szArtExt[e] );
            WIN32_FILE_ATTRIBUTE_DATA Attr;
            if( !GetFileAttributesExA( szPath, GetFileExInfoStandard, &Attr ) )
                continue;
            const ArtStamp Stamp = { Attr.nFileSizeLow, Attr.ftLastWriteTime.dwLowDateTime,
                                     Attr.ftLastWriteTime.dwHighDateTime };
            char szCache[MAX_PATH], szBad[MAX_PATH];
            sprintf_s( szCache, "%sCache\\%s.dxt", c_szCoverDirs[d], it.szTitleId );
            sprintf_s( szBad, "%sCache\\%s.%s.bad", c_szCoverDirs[d], it.szTitleId, c_szArtExt[e] );

            D3DTexture* pTex = LoadCachedArt( pDevice, szCache, Stamp );
            if( !pTex )
            {
                if( IsMarkedBad( szBad, Stamp ) )
                    continue;
                if( !bDecode )
                {
                    s_Art[nShelf][i] = ART_DECODE;
                    return;
                }
                // Scaled to a power-of-two size so its mip levels are exactly half the
                // level above, which is how LevelBytes and the cache lay them out (box
                // scans are 900 x 600; the GPU pads such a texture's mips, which scrambled
                // the cached covers). Each side goes to the nearest power of two, at most
                // 1024; a box scan is kept 2:1 (900 x 600 -> 1024 x 512) and a front never
                // wider than tall, so wide still means wrap below. Levels down to the
                // packed tail, so the result can be cached.
                D3DXIMAGE_INFO Info;
                HRESULT hr = D3DXGetImageInfoFromFileA( szPath, &Info );
                if( FAILED( hr ) )
                {
                    if( MemoryShort( hr ) )
                    {
                        s_ArtRetry[nShelf][i] = TRUE;
                        return;
                    }
                    MarkBad( c_szCoverDirs[d], szBad, Stamp );
                    continue;
                }
                const BOOL bWide = Info.Width > Info.Height * 1.2f;
                const UINT w = NearestPow2( Info.Width );
                const UINT h = bWide ? w / 2 : max( w, NearestPow2( Info.Height ) );
                const UINT nLevels = ArtLevels( w, h );
                hr = D3DXCreateTextureFromFileExA( pDevice, szPath, w, h, nLevels, 0, D3DFMT_DXT1, D3DPOOL_MANAGED,
                                                   D3DX_FILTER_LINEAR, D3DX_FILTER_BOX, 0, NULL, NULL, &pTex );   // the default (triangle) filter is far slower
                if( FAILED( hr ) )
                {
                    if( MemoryShort( hr ) )
                    {
                        s_ArtRetry[nShelf][i] = TRUE;
                        return;
                    }
                    MarkBad( c_szCoverDirs[d], szBad, Stamp );
                    continue;
                }
                SaveCachedArt( c_szCoverDirs[d], szCache, Stamp, pTex );
            }
            D3DSURFACE_DESC Desc;
            pTex->GetLevelDesc( 0, &Desc );
            s_pArt[nShelf][i] = pTex;
            s_Art[nShelf][i] = ( Desc.Width > Desc.Height * 1.2f ) ? ART_WRAP : ART_FRONT;
            s_dwArtKey[nShelf][i] = ArtKey( it.szTitleId );
            s_ArtStamp[nShelf][i] = Stamp;
            return;
        }
}

// Whether X can turn this item round: boxed shelves only (a box scan, or a drawn back)
static BOOL HasBack( int nShelf, int i )
{
    if( !c_bShelfBoxed[nShelf] )
        return FALSE;
    if( s_Art[nShelf][i] == ART_WRAP )
        return TRUE;
    return c_bShelfBacks[nShelf];
}


//-------------------------------------------------------------------------------------
// Drawn covers come and go with the view. One the GPU may still be drawing waits here
// until a fence says it's done with it.
//-------------------------------------------------------------------------------------
struct Dropped { D3DTexture* pTex; DWORD dwFence; };
static const int MAX_DROPPED = 64;
static Dropped   s_Dropped[MAX_DROPPED];
static int       s_nDropped = 0;

static void DropTexture( D3DDevice* pDevice, D3DTexture*& pTex )
{
    if( !pTex )
        return;
    if( s_nDropped == MAX_DROPPED )
    {
        // Full: wait for the oldest (the GPU is a frame or two behind at most)
        pDevice->BlockOnFence( s_Dropped[0].dwFence );
        s_Dropped[0].pTex->Release();
        memmove( s_Dropped, s_Dropped + 1, ( MAX_DROPPED - 1 ) * sizeof(Dropped) );
        s_nDropped--;
    }
    s_Dropped[s_nDropped].pTex    = pTex;
    s_Dropped[s_nDropped].dwFence = pDevice->InsertFence();   // after every draw that used it
    s_nDropped++;
    pTex = NULL;
}

static void ReleaseDropped( D3DDevice* pDevice )
{
    int k = 0;
    for( int j = 0; j < s_nDropped; j++ )
    {
        if( pDevice->IsFencePending( s_Dropped[j].dwFence ) )
            s_Dropped[k++] = s_Dropped[j];
        else
            s_Dropped[j].pTex->Release();
    }
    s_nDropped = k;
}

// Drawn covers are kept for what's on screen or about to be: the last open shelf's
// cases within KEEP_NEAR of its front one (SHOWN_EACH_SIDE show each side; the rest are
// made ahead so they're ready before they slide in) and every shelf's first few (what
// it shows when it opens). They're dropped only past DROP_FAR, so stepping back and
// forth doesn't remake them.
static const int KEEP_NEAR = SHOWN_EACH_SIDE + 2, DROP_FAR = SHOWN_EACH_SIDE + 5;
static const int HOME_ITEMS = SHOWN_EACH_SIDE + 1;
static const int DRAWN_PER_FRAME = 4;

static BOOL WantDrawn( int nShelf, int i, int nReach )
{
    return i < HOME_ITEMS || ( nShelf == s_nShelf && abs( i - s_nIndex ) <= nReach );
}

// Makes an item's missing drawn cover (no box art) and back (boxed shelves, unless its
// art wraps the case); returns how many it made, at most nBudget. Nothing until it's
// known whether the item has art.
static int EnsureDrawn( D3DDevice* pDevice, int nShelf, int i, int nBudget )
{
    const BYTE art = s_Art[nShelf][i];
    if( art == ART_UNTRIED || art == ART_DECODE )
        return 0;
    int nMade = 0;
    BYTE& failed = s_Failed[nShelf][i];
    const ScanItem& it = Scan_GetItem( nShelf, i );
    if( art == ART_NONE && !s_pCover[nShelf][i] && !( failed & FAILED_FRONT ) && nMade < nBudget )
    {
        // A cover that can't be made (out of memory) is given up on, not retried every frame
        if( !( s_pCover[nShelf][i] = MakeCover( pDevice, it, FALSE ) ) )
            failed |= FAILED_FRONT;
        nMade++;
    }
    if( art != ART_WRAP && c_bShelfBacks[nShelf] && !s_pBack[nShelf][i] && !( failed & FAILED_BACK ) && nMade < nBudget )
    {
        if( !( s_pBack[nShelf][i] = MakeCover( pDevice, it, TRUE ) ) )
            failed |= FAILED_BACK;
        nMade++;
    }
    return nMade;
}


//-------------------------------------------------------------------------------------
// A rescan: the shelves' items move about, so their textures are matched up again
//-------------------------------------------------------------------------------------
static BOOL s_bRemapPending = FALSE;    // the library changed: covers follow before the next frame
static char s_szDeletingId[12] = "";    // the title ID of the game our delete job is removing
static char s_szDeletedId[12]  = "";    // ...once it's gone: PruneCache deletes its covers after the rescan

struct OldArt { D3DTexture* pTex; BYTE art; DWORD dwKey; ArtStamp Stamp; BOOL bIcon; };
static OldArt s_OldArt[NUM_SHELVES * MAX_SHELF];

// Drawn covers are let go (they're quick to make again, near the view). Box art is kept
// for every new item showing the same image: same title ID, and the image file not
// replaced since. The rest of the old art is released.
static void RemapCovers( D3DDevice* pDevice )
{
    // The GPU may still be drawing last frame's covers
    pDevice->BlockUntilIdle();
    ReleaseDropped( pDevice );

    int nOld = 0;
    for( int s = 0; s < NUM_SHELVES; s++ )
        for( int i = 0; i < MAX_SHELF; i++ )
        {
            if( s_pCover[s][i] ) { s_pCover[s][i]->Release(); s_pCover[s][i] = NULL; }
            if( s_pBack[s][i] )  { s_pBack[s][i]->Release();  s_pBack[s][i] = NULL; }
            if( s_pArt[s][i] )
            {
                OldArt& o = s_OldArt[nOld++];
                o.pTex  = s_pArt[s][i];
                o.art   = s_Art[s][i];
                o.dwKey = s_dwArtKey[s][i];
                o.Stamp = s_ArtStamp[s][i];
                o.bIcon = s == SHELF_ARCADE;
                s_pArt[s][i] = NULL;
            }
            s_Art[s][i] = ART_UNTRIED;
            s_Failed[s][i] = 0;
        }

    for( int s = 0; s < NUM_SHELVES && nOld; s++ )
        for( int i = 0; i < ShelfCount( s ); i++ )
        {
            const ScanItem& it = Scan_GetItem( s, i );
            if( !it.szTitleId[0] )
                continue;
            const DWORD dwKey = ArtKey( it.szTitleId );
            ArtStamp Stamp = { 0, 0, 0 };
            BOOL bStamp = FALSE;   // looked up once, and only if there's art to compare with
            for( int k = 0; k < nOld; k++ )
            {
                const OldArt& o = s_OldArt[k];
                if( o.dwKey != dwKey || o.bIcon != ( s == SHELF_ARCADE ) )
                    continue;
                if( !o.bIcon )
                {
                    if( !bStamp )
                    {
                        FindArtFile( it.szTitleId, &Stamp );   // none: stays zero, so no match
                        bStamp = TRUE;
                    }
                    if( memcmp( &Stamp, &o.Stamp, sizeof(Stamp) ) != 0 )
                        continue;   // the image was replaced (or removed): load it again
                }
                s_pArt[s][i] = o.pTex;
                s_pArt[s][i]->AddRef();
                s_Art[s][i]      = o.art;
                s_dwArtKey[s][i] = o.dwKey;
                s_ArtStamp[s][i] = o.Stamp;
                break;
            }
        }

    // Each slot held one reference; the ones kept were taken again above
    for( int k = 0; k < nOld; k++ )
        s_OldArt[k].pTex->Release();
}

static BOOL InLibrary( const char* szTitleId )
{
    for( int s = 0; s < NUM_SHELVES; s++ )
        for( int i = 0; i < Scan_ItemCount( s ); i++ )
            if( _stricmp( Scan_GetItem( s, i ).szTitleId, szTitleId ) == 0 )
                return TRUE;
    return FALSE;
}

// After a rescan, deletes cache files nothing will use again: leftovers of an
// interrupted save (.tmp) and ones whose image is gone. A game Orbit just deleted loses
// its images and icon first, unless another copy is still in the library. Games only
// missing from the library keep theirs: their drive may just be unplugged. Names are
// gathered first and deleted after, a batch at a time.
static void PruneCache()
{
    if( s_szDeletedId[0] && !InLibrary( s_szDeletedId ) )
    {
        char szPath[MAX_PATH];
        for( int d = 0; d < ARRAYSIZE( c_szCoverDirs ); d++ )
            for( int e = 0; e < ARRAYSIZE( c_szArtExt ); e++ )
            {
                sprintf_s( szPath, "%s%s.%s", c_szCoverDirs[d], s_szDeletedId, c_szArtExt[e] );
                DeleteFileA( szPath );
            }
        sprintf_s( szPath, "game:\\Covers\\Icons\\%s.png", s_szDeletedId );
        DeleteFileA( szPath );
    }
    s_szDeletedId[0] = 0;

    static const int BATCH = 32;
    for( int d = 0; d < ARRAYSIZE( c_szCoverDirs ); d++ )
    {
        char szFind[MAX_PATH];
        sprintf_s( szFind, "%sCache\\*", c_szCoverDirs[d] );
        for( ;; )
        {
            static char szDelete[BATCH][MAX_PATH];   // (8 KB: not on the stack)
            int nDelete = 0;
            WIN32_FIND_DATAA fd;
            HANDLE hFind = FindFirstFileA( szFind, &fd );
            if( hFind == INVALID_HANDLE_VALUE )
                break;
            do
            {
                if( fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY )
                    continue;
                // <title ID>.dxt, <title ID>.<ext>.bad, or either with .tmp after; others aren't ours
                const char* szName = fd.cFileName;
                size_t n = strlen( szName );
                BOOL bTmp = n > 4 && _stricmp( szName + n - 4, ".tmp" ) == 0;
                BOOL bOurs = bTmp || ( n > 4 && ( _stricmp( szName + n - 4, ".dxt" ) == 0 || _stricmp( szName + n - 4, ".bad" ) == 0 ) );
                if( !bOurs )
                    continue;
                BOOL bDelete = bTmp;
                if( !bDelete )
                {
                    char szId[16];
                    size_t nId = strcspn( szName, "." );
                    if( nId == 0 || nId >= sizeof(szId) )
                        continue;
                    memcpy( szId, szName, nId );
                    szId[nId] = 0;
                    bDelete = !FindArtFile( szId, NULL );
                }
                if( bDelete )
                    sprintf_s( szDelete[nDelete++], "%sCache\\%s", c_szCoverDirs[d], szName );
            }
            while( nDelete < BATCH && FindNextFileA( hFind, &fd ) );
            FindClose( hFind );

            int nGone = 0;
            for( int k = 0; k < nDelete; k++ )
                if( DeleteFileA( szDelete[k] ) )
                    nGone++;
            if( nDelete < BATCH || nGone == 0 )
                break;   // none deleted (read-only): the same batch would come back forever
        }
    }
}


//-------------------------------------------------------------------------------------
// Public: covers
//-------------------------------------------------------------------------------------
int Library_CoversRemaining( int* pnTotal )
{
    int nTotal = 0, nLeft = 0;
    for( int s = 0; s < NUM_SHELVES; s++ )
        for( int i = 0; i < ShelfCount( s ); i++ )
        {
            nTotal++;
            const BYTE art = s_Art[s][i];
            if( s_bRemapPending || art == ART_UNTRIED || art == ART_DECODE )
                nLeft++;
        }
    if( pnTotal )
        *pnTotal = nTotal;
    return nLeft;
}

void Library_PrepareCovers( D3DDevice* pDevice )
{
    if( s_bRemapPending )
    {
        RemapCovers( pDevice );
        s_bRemapPending = FALSE;
        PruneCache();
    }
    ReleaseDropped( pDevice );

    // Box art still to load: cache reads while the frame has time, and at most one
    // decode (the slow part, a good fraction of a second), which ends the frame's share
    static const DWORD ART_MS = 10;
    const DWORD dwStart = GetTickCount();
    BOOL bBusy = FALSE;
    for( int s = 0; s < NUM_SHELVES && !bBusy; s++ )
        for( int i = 0; i < ShelfCount( s ) && !bBusy; i++ )
        {
            const BYTE art = s_Art[s][i];
            if( art != ART_UNTRIED && art != ART_DECODE )
                continue;
            LoadArt( pDevice, s, i, art == ART_DECODE );
            if( s == s_nShelf && i == s_nIndex )
                s_bHudDirty = TRUE;   // X BACK COVER may now apply
            bBusy = art == ART_DECODE || GetTickCount() - dwStart >= ART_MS;
        }

    // Art that ran out of memory: retried now and then, never with the shelves up (it'd hitch them)
    static const DWORD RETRY_MS = 10000;
    static DWORD s_dwLastRetry = 0;
    if( !bBusy && !s_bOpen && s_fLibScale <= 0.01f && GetTickCount() - s_dwLastRetry >= RETRY_MS && RoomToDecode() )
    {
        BOOL bTried = FALSE;
        for( int s = 0; s < NUM_SHELVES && !bTried; s++ )
            for( int i = 0; i < ShelfCount( s ) && !bTried; i++ )
            {
                if( !s_ArtRetry[s][i] || s_Art[s][i] != ART_NONE )
                    continue;
                bTried = TRUE;
                s_dwLastRetry = GetTickCount();
                LoadArt( pDevice, s, i, TRUE );
                if( s_Art[s][i] == ART_NONE )
                    continue;
                DropTexture( pDevice, s_pCover[s][i] );
                if( s_Art[s][i] == ART_WRAP )
                    DropTexture( pDevice, s_pBack[s][i] );
                if( s == s_nShelf && i == s_nIndex )
                    s_bHudDirty = TRUE;
            }
    }

    // Drawn covers: let go of the ones far from view...
    for( int s = 0; s < NUM_SHELVES; s++ )
        for( int i = 0; i < MAX_SHELF; i++ )
            if( ( s_pCover[s][i] || s_pBack[s][i] ) && ( i >= ShelfCount( s ) || !WantDrawn( s, i, DROP_FAR ) ) )
            {
                DropTexture( pDevice, s_pCover[s][i] );
                DropTexture( pDevice, s_pBack[s][i] );
                s_Failed[s][i] = 0;
            }

    // ...and make the near ones: the last open shelf's nearest the front first, then
    // each shelf's first few
    int nMade = 0;
    const int n = ShelfCount( s_nShelf );
    for( int d = 0; d <= KEEP_NEAR && nMade < DRAWN_PER_FRAME; d++ )
        for( int sg = -1; sg <= 1 && nMade < DRAWN_PER_FRAME; sg += 2 )
        {
            int i = s_nIndex + d * sg;
            if( i < 0 || i >= n || ( d == 0 && sg > 0 ) )
                continue;
            nMade += EnsureDrawn( pDevice, s_nShelf, i, DRAWN_PER_FRAME - nMade );
        }
    for( int s = 0; s < NUM_SHELVES && nMade < DRAWN_PER_FRAME; s++ )
        for( int i = 0; i < min( ShelfCount( s ), HOME_ITEMS ) && nMade < DRAWN_PER_FRAME; i++ )
            nMade += EnsureDrawn( pDevice, s, i, DRAWN_PER_FRAME - nMade );
}


//-------------------------------------------------------------------------------------
// HUD text for the current state
//-------------------------------------------------------------------------------------
static void UpdateHud()
{
    if( !s_bOpen )
    {
        Hud_SetGameInfo( NULL, NULL );
        Hud_SetDetails( 0, NULL, NULL, NULL, 0 );
        Hud_SetMenuName( NULL );
        Hud_SetMainKeys();
        return;
    }

    Hud_SetMenuName( c_szShelfNames[s_nShelf] );
    const int n = ItemCount();
    if( n == 0 )
    {
        Hud_SetGameInfo( Scan_IsRunning() ? "SCANNING..." : "NOTHING FOUND",
                         Scan_IsRunning() ? "" : "CHECK THE CONTENT PATHS IN SETTINGS" );
        Hud_SetDetails( 0, NULL, NULL, NULL, 0 );
        static const HudKey c_Empty[] = { { HUD_POD_B, "BACK" } };
        Hud_SetKeys( c_Empty, ARRAYSIZE( c_Empty ) );
        return;
    }

    // Just the title above the shelf; version and size are on the details card
    const ScanItem& it = Scan_GetItem( s_nShelf, s_nIndex );
    char szTitle[96], szSize[32];
    UpperCopy( szTitle, sizeof(szTitle), it.szTitle );
    FormatSize( it.qwSize, szSize, sizeof(szSize) );
    Hud_SetGameInfo( szTitle, NULL );

    static const HudKey c_KeysBacks[] =
    {
        { HUD_POD_NONE, "\x01 \x02 BROWSE" }, { HUD_POD_A, "LAUNCH" }, { HUD_POD_X, "BACK COVER" },
        { HUD_POD_Y, "DETAILS" }, { HUD_POD_B, "BACK" },
    };
    static const HudKey c_KeysNoBacks[] =
    {
        { HUD_POD_NONE, "\x01 \x02 BROWSE" }, { HUD_POD_A, "LAUNCH" },
        { HUD_POD_Y, "DETAILS" }, { HUD_POD_B, "BACK" },
    };
    if( HasBack( s_nShelf, s_nIndex ) ) Hud_SetKeys( c_KeysBacks, ARRAYSIZE( c_KeysBacks ) );
    else                                Hud_SetKeys( c_KeysNoBacks, ARRAYSIZE( c_KeysNoBacks ) );

    if( s_bDetails )
    {
        static const char* c_szLabels[] = { "TITLE", "VERSION", "SIZE", "STATUS" };
        char szStatus[48] = "";
        if( s_bDeleting )
        {
            JobStatus Job;
            Drives_GetJob( &Job );
            int nPct = Job.qwTotal ? (int)( 100.0 * (double)Job.qwDone / (double)Job.qwTotal ) : 0;
            sprintf_s( szStatus, "DELETING %d%%", nPct );
        }
        const char* szValues[] = { szTitle, it.szVersion, szSize, szStatus };
        static const HudKey c_Card[]    = { { HUD_POD_X, "DELETE" }, { HUD_POD_B, "CLOSE" } };
        static const HudKey c_Confirm[] = { { HUD_POD_X, "PRESS AGAIN TO DELETE" }, { HUD_POD_B, "CANCEL" } };
        static const HudKey c_Busy[]    = { { HUD_POD_B, "STOP" } };
        if( s_bDeleting )     Hud_SetDetails( 4, c_szLabels, szValues, c_Busy, ARRAYSIZE( c_Busy ) );
        else if( s_bConfirm ) Hud_SetDetails( 3, c_szLabels, szValues, c_Confirm, ARRAYSIZE( c_Confirm ) );
        else                  Hud_SetDetails( 3, c_szLabels, szValues, c_Card, ARRAYSIZE( c_Card ) );
    }
    else
    {
        Hud_SetDetails( 0, NULL, NULL, NULL, 0 );
    }
}


//-------------------------------------------------------------------------------------
// Name: Library_Open() / Library_IsOpen() / Library_OnLibraryChanged()
//-------------------------------------------------------------------------------------
BOOL Library_Open( const char* szRingLabel )
{
    int nShelf = -1;
    for( int s = 0; s < NUM_SHELVES; s++ )
        if( strcmp( szRingLabel, c_szShelfNames[s] ) == 0 )
            nShelf = s;
    if( nShelf < 0 )
        return FALSE;

    if( nShelf != s_nShelf )
    {
        s_nShelf = nShelf;
        s_nIndex = 0;
    }
    s_nIndex = max( 0, min( s_nIndex, ItemCount() - 1 ) );
    for( int i = 0; i < MAX_SHELF; i++ )
    {
        CaseState& cs = s_Cases[i];
        cs.x = cs.y = cs.z = cs.fRotY = 0.0f;
        cs.fScale = 1.0f; cs.fLevel = 1.0f;
    }
    s_bOpen = TRUE;
    s_bFlipped = s_bDetails = s_bConfirm = FALSE;
    UpdateHud();
    return TRUE;
}

BOOL Library_IsOpen() { return s_bOpen; }

void Library_OnLibraryChanged()
{
    s_bRemapPending = TRUE;
    s_nIndex = max( 0, min( s_nIndex, ItemCount() - 1 ) );
    s_bDetails = s_bConfirm = s_bFlipped = FALSE;
    if( s_bOpen )
        UpdateHud();
}


//-------------------------------------------------------------------------------------
// Name: Library_Update()
//-------------------------------------------------------------------------------------
void Library_Update( float dt, int nMove, WORD wPressed, float* pfPulse )
{
    s_fTime += dt;
    const int n = ItemCount();

    if( s_bOpen )
    {
        BOOL bChanged = FALSE;

        if( s_bDeleting )
        {
            JobStatus Job;
            Drives_GetJob( &Job );
            if( Job.nState == JOBSTATE_RUNNING )
            {
                if( wPressed & XINPUT_GAMEPAD_B )
                    Drives_CancelJob();
                UpdateHud();   // progress
                wPressed = 0;
                nMove = 0;
            }
            else
            {
                s_bDeleting = FALSE;
                s_bDetails = FALSE;
                char sz[128];
                if( Job.nState == JOBSTATE_DONE )           sprintf_s( sz, "DELETED %s", Job.szName );
                else if( Job.nState == JOBSTATE_CANCELLED ) strcpy_s( sz, "DELETE STOPPED" );
                else                                        sprintf_s( sz, "FAILED: %s", Job.szError );
                char szUp[128];
                UpperCopy( szUp, sizeof(szUp), sz );
                Hud_ShowToast( szUp, 2.2f );
                if( Job.nState == JOBSTATE_DONE )
                    strcpy_s( s_szDeletedId, s_szDeletingId );   // PruneCache deletes its covers after the rescan
                Drives_ClearJob();
                Drives_Refresh();
                Scan_Start();   // the shelf updates when the scan finishes
                bChanged = TRUE;
            }
        }
        else if( s_bDetails )
        {
            if( wPressed & ( XINPUT_GAMEPAD_B | XINPUT_GAMEPAD_Y ) )
            {
                s_bDetails = s_bConfirm = FALSE;
                bChanged = TRUE;
            }
            else if( ( wPressed & XINPUT_GAMEPAD_X ) && n )
            {
                const ScanItem& it = Scan_GetItem( s_nShelf, s_nIndex );
                if( Drives_IsProtected( it.szDelete ) )
                {
                    Hud_ShowToast( "PROTECTED - ORBIT WON'T DELETE THIS", 2.2f );
                    s_bConfirm = FALSE;
                }
                else if( !s_bConfirm )
                    s_bConfirm = TRUE;
                else
                {
                    s_bConfirm = FALSE;
                    char szWhy[64];
                    if( Drives_StartDelete( it.szDelete, szWhy, sizeof(szWhy) ) )
                    {
                        s_bDeleting = TRUE;
                        strcpy_s( s_szDeletingId, it.szTitleId );   // its covers go after the rescan
                    }
                    else
                    {
                        char sz[128];
                        sprintf_s( sz, "CAN'T DELETE: %s", szWhy );
                        Hud_ShowToast( sz, 2.2f );
                    }
                }
                bChanged = TRUE;
            }
        }
        else if( wPressed & XINPUT_GAMEPAD_B )
        {
            s_bOpen = FALSE;
            bChanged = TRUE;
        }
        else if( n )
        {
            int ng = max( 0, min( n - 1, s_nIndex + nMove ) );
            if( ng != s_nIndex )
            {
                s_nIndex = ng;
                s_bFlipped = FALSE;
                bChanged = TRUE;
            }
            if( wPressed & XINPUT_GAMEPAD_A )
            {
                *pfPulse = 1.0f;
                const ScanItem& it = Scan_GetItem( s_nShelf, s_nIndex );
                // A loose .xex / .xbe launches as it is; an installed game (a Games on
                // Demand or Arcade package) has its default.xex launched from inside it
                size_t n = strlen( it.szLaunch );
                BOOL bFile = n > 4 && ( _stricmp( it.szLaunch + n - 4, ".xex" ) == 0 || _stricmp( it.szLaunch + n - 4, ".xbe" ) == 0 );
                char szToast[80];
                if( bFile )
                {
                    XLaunchNewImage( it.szLaunch, 0 );   // only returns if the launch failed
                    sprintf_s( szToast, "COULDN'T LAUNCH THAT (ERROR %08X)", GetLastError() );
                }
                else
                {
                    // Freestyle Dash's way: mount the package as a drive (orbitpkg:\) and
                    // launch the default.xex inside it like any other. (Launching the
                    // package file itself crashes to the dashboard, and
                    // XContentLaunchImageFromFile seemed to stall rather than fail in
                    // testing, which would hide the error.)
                    char szMount[MAX_PATH];
                    sprintf_s( szMount, "\\??\\%s", it.szLaunch );
                    DWORD dwErr = (DWORD)XamContentOpenFile( 0xFE, "orbitpkg", szMount, 0x4000043, 0, 0, NULL );
                    if( dwErr == 0 )
                    {
                        XLaunchNewImage( "orbitpkg:\\default.xex", 0 );   // only returns if it failed
                        sprintf_s( szToast, "COULDN'T LAUNCH THAT (LAUNCH %08X)", GetLastError() );
                        XamContentClose( "\\??\\orbitpkg", NULL );
                    }
                    else
                        sprintf_s( szToast, "COULDN'T LAUNCH THAT (MOUNT %08X)", dwErr );
                }
                Hud_ShowToast( szToast, 5.0f );
            }
            if( ( wPressed & XINPUT_GAMEPAD_X ) && HasBack( s_nShelf, s_nIndex ) )
                s_bFlipped = !s_bFlipped;
            if( wPressed & XINPUT_GAMEPAD_Y )
            {
                s_bDetails = TRUE;
                bChanged = TRUE;
            }
        }
        if( bChanged || s_bHudDirty )
            UpdateHud();
        s_bHudDirty = FALSE;
    }

    s_fLibScale = Damp( s_fLibScale, s_bOpen ? 1.0f : 0.001f, 7.0f, dt );
    if( s_fLibScale <= 0.01f )
        return;

    const float fSway = sinf( s_fTime * 0.8f ) * 0.12f;
    for( int i = 0; i < n; i++ )
    {
        int d = i - s_nIndex, ad = abs( d ), sg = Sign( d );
        float tx, tz, ry, ts;
        if( c_bShelfBoxed[s_nShelf] )
        {
            tx = ( d == 0 ) ? 0.0f : sg * ( 0.95f + ( ad - 1 ) * 0.5f );
            tz = ( d == 0 ) ? 0.55f : -0.15f - ad * 0.08f;
            // Every side case turns the same way, so its spine faces the camera on both sides
            ry = ( d == 0 ) ? fSway + ( s_bFlipped ? XM_PI : 0.0f ) : 0.95f;
            ts = ( d == 0 ) ? 1.18f : 0.92f;
        }
        else
        {
            // Tiles face the camera and spread out enough not to overlap
            tx = ( d == 0 ) ? 0.0f : sg * ( 1.0f + ( ad - 1 ) * 0.85f );
            tz = ( d == 0 ) ? 0.55f : -0.15f - ad * 0.08f;
            ry = ( d == 0 ) ? fSway : 0.0f;
            ts = ( d == 0 ) ? 1.18f : 0.8f;
        }
        CaseState& cs = s_Cases[i];
        cs.x      = Damp( cs.x, tx, 9.0f, dt );
        cs.z      = Damp( cs.z, tz, 9.0f, dt );
        cs.y      = ( d == 0 ) ? sinf( s_fTime * 1.1f ) * 0.03f : 0.0f;
        cs.fRotY  = Damp( cs.fRotY, ry, 9.0f, dt );
        cs.fScale = Damp( cs.fScale, ts, 9.0f, dt );
        cs.fLevel = Damp( cs.fLevel, ( d == 0 ) ? 1.0f : 0.55f, 9.0f, dt );
    }
}


//-------------------------------------------------------------------------------------
// Name: Library_Render()
//-------------------------------------------------------------------------------------
void Library_Render( D3DDevice* pDevice, const XMMATRIX& matView, const XMMATRIX& matProj )
{
    if( s_fLibScale <= 0.01f )
        return;
    const int n = ItemCount();

    XMMATRIX matLib = XMMatrixScaling( s_fLibScale, s_fLibScale, s_fLibScale ) * XMMatrixTranslation( 0.0f, 0.3f, 2.2f );

    // Visible cases, far to near
    // (static: a shelf can be long, and they'd be a lot for the stack)
    static int      nOrder[MAX_SHELF];
    static float    fDepth[MAX_SHELF];
    static XMMATRIX matWorld[MAX_SHELF];
    int nItems = 0;
    for( int i = max( 0, s_nIndex - SHOWN_EACH_SIDE ); i < min( n, s_nIndex + SHOWN_EACH_SIDE + 1 ); i++ )
    {
        const CaseState& cs = s_Cases[i];
        const float fThick = c_bShelfBoxed[s_nShelf] ? 1.0f : TILE_DEPTH;   // tiles are thin
        const float fTall  = c_bShelfSquare[s_nShelf] ? CASE_W / CASE_H : 1.0f;
        matWorld[i] = XMMatrixScaling( cs.fScale, cs.fScale * fTall, cs.fScale * fThick ) * XMMatrixRotationY( cs.fRotY )
                    * XMMatrixTranslation( cs.x, cs.y, cs.z ) * matLib;
        fDepth[i] = -XMVectorGetZ( XMVector3Transform( XMVectorZero(), matWorld[i] * matView ) );
        int j = nItems++;
        for( ; j > 0 && fDepth[nOrder[j - 1]] < fDepth[i]; j-- )
            nOrder[j] = nOrder[j - 1];
        nOrder[j] = i;
    }
    if( !nItems )
        return;

    float vTrims[4];
    Post_GetTrims( vTrims );

    pDevice->SetRenderState( D3DRS_ZENABLE, D3DZB_FALSE );
    pDevice->SetRenderState( D3DRS_ZWRITEENABLE, FALSE );
    pDevice->SetRenderState( D3DRS_ALPHABLENDENABLE, TRUE );
    pDevice->SetRenderState( D3DRS_BLENDOP, D3DBLENDOP_ADD );
    pDevice->SetRenderState( D3DRS_SRCBLEND, D3DBLEND_ONE );
    pDevice->SetRenderState( D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA );
    pDevice->SetVertexDeclaration( s_pDecl );
    pDevice->SetVertexShader( s_pVS );
    pDevice->SetPixelShader( s_pPS );
    pDevice->SetPixelShaderConstantF( 2, vTrims, 1 );
    pDevice->SetSamplerState( 0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR );
    pDevice->SetSamplerState( 0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR );
    pDevice->SetSamplerState( 0, D3DSAMP_MIPFILTER, D3DTEXF_LINEAR );   // box art has mips; drawn covers have one level
    pDevice->SetSamplerState( 0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP );
    pDevice->SetSamplerState( 0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP );

    for( int k = 0; k < nItems; k++ )
    {
        const int i = nOrder[k];
        const CaseState& cs = s_Cases[i];
        const int t = TemplateFor( Scan_GetItem( s_nShelf, i ).szTitle );
        XMMATRIX matWV  = matWorld[i] * matView;
        XMMATRIX matWVP = matWV * matProj;
        pDevice->SetVertexShaderConstantF( 0, (const float*)&matWVP, 4 );
        pDevice->SetVertexShaderConstantF( 4, (const float*)&matWV, 4 );

        // Faces: front faces only (FrontSide), so the box needs no sorting of its own
        pDevice->SetRenderState( D3DRS_CULLMODE, D3DCULL_CW );
        const BYTE art = s_Art[s_nShelf][i];
        D3DTexture* pArt = s_pArt[s_nShelf][i];

        // This case's plastic: by kind, or the drawn cover's hue for apps
        float vEdge[4], vPlastic[4], vLip[4];
        const CaseLook& look = c_Looks[s_nShelf];
        if( s_nShelf == SHELF_APPS )
        {
            memcpy( vEdge, s_vEdge[t], sizeof(vEdge) );
            memcpy( vPlastic, s_vPlastic[t], sizeof(vPlastic) );
            memcpy( vLip, s_vEdge[t], sizeof(vLip) );
        }
        else
        {
            for( int c = 0; c < 3; c++ )
            {
                vEdge[c]    = SRGBToLinear( look.Edge[c] / 255.0f );
                vPlastic[c] = SRGBToLinear( look.Plastic[c] / 255.0f );
                vLip[c]     = SRGBToLinear( look.Lip[c] / 255.0f );
            }
            vEdge[3] = vPlastic[3] = vLip[3] = 1.0f;
        }
        pDevice->SetPixelShaderConstantF( 5, vLip, 1 );

        for( int f = 0; f < NUM_FACES; f++ )
        {
            D3DTexture* pTex = NULL;
            float vColor[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
            float vUV[4] = { 1.0f, 1.0f, 0.0f, 0.0f };   // whole map
            float vLipParams[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
            switch( f )
            {
            case FACE_PZ:
                if( art == ART_WRAP )       { pTex = pArt; vUV[0] = 1.0f - WRAP_SPINE_R; vUV[2] = WRAP_SPINE_R; }
                else if( art == ART_FRONT || art == ART_ICON ) pTex = pArt;
                else                                           pTex = s_pCover[s_nShelf][i];
                vColor[0] = vColor[1] = vColor[2] = cs.fLevel;
                break;
            case FACE_NZ:
                if( art == ART_WRAP )                pTex = pArt, vUV[0] = WRAP_SPINE_L;
                else if( c_bShelfBacks[s_nShelf] )   pTex = s_pBack[s_nShelf][i];
                vColor[0] = vColor[1] = vColor[2] = cs.fLevel;
                break;
            case FACE_NX:
                if( art == ART_WRAP ) { pTex = pArt; vUV[0] = WRAP_SPINE_R - WRAP_SPINE_L; vUV[2] = WRAP_SPINE_L; }
                else                  memcpy( vColor, vPlastic, sizeof(vColor) );
                break;
            case FACE_NY: memcpy( vColor, vPlastic, sizeof(vColor) ); break;
            default:      memcpy( vColor, vEdge, sizeof(vColor) ); break;
            }
            // Covers sit in the case's lip under the sleeve's sheen
            if( pTex && ( f == FACE_PZ || f == FACE_NZ ) && s_nShelf != SHELF_APPS )
            {
                // A square face gets the same lip all round (LIP_U: a fraction of its width)
                vLipParams[0] = LIP_U; vLipParams[1] = c_bShelfSquare[s_nShelf] ? LIP_U : LIP_V; vLipParams[2] = SHEEN;
            }
            // No art (yet, or a shelf without backs): the plastic colour
            if( !pTex && ( f == FACE_PZ || f == FACE_NZ ) )
                memcpy( vColor, vPlastic, sizeof(vColor) );
            // An icon's see-through parts show the lip's plastic
            float vMode[4] = { pTex ? 1.0f : 0.0f, ( pTex && art == ART_ICON ) ? 1.0f : 0.0f, 0.0f, 0.0f };
            pDevice->SetPixelShaderConstantF( 0, vColor, 1 );
            pDevice->SetPixelShaderConstantF( 1, vMode, 1 );
            pDevice->SetPixelShaderConstantF( 3, vUV, 1 );
            pDevice->SetPixelShaderConstantF( 4, vLipParams, 1 );
            pDevice->SetTexture( 0, pTex );
            pDevice->DrawPrimitiveUP( D3DPT_TRIANGLESTRIP, 2, s_Faces[f], sizeof(CaseVertex) );
        }
    }

    pDevice->SetTexture( 0, NULL );
    pDevice->SetRenderState( D3DRS_ALPHABLENDENABLE, FALSE );
}
