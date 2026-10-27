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
// title) with the title written on it, rendered to a 256 x 352 texture a couple per
// frame as they come into view.
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

static const int   MAX_SHELF = 64;
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
enum { ART_UNTRIED, ART_NONE, ART_FRONT, ART_WRAP, ART_ICON };   // ART_ICON: an ARCADE package's icon
static BOOL        s_bHudDirty = FALSE;                // art arrived for the front case
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

static void ReleaseCovers()
{
    for( int s = 0; s < NUM_SHELVES; s++ )
        for( int i = 0; i < MAX_SHELF; i++ )
        {
            if( s_pCover[s][i] ) { s_pCover[s][i]->Release(); s_pCover[s][i] = NULL; }
            if( s_pBack[s][i] )  { s_pBack[s][i]->Release();  s_pBack[s][i] = NULL; }
            if( s_pArt[s][i] )   { s_pArt[s][i]->Release();   s_pArt[s][i] = NULL; }
            s_Art[s][i] = ART_UNTRIED;
        }
}

// Box art cache: Covers\Cache\<title ID>.dxt beside the image, holding the texture
// D3DX made from it so later boots skip the decode. Big-endian, the 360's order:
//
//   "OCV1", image size, image time (low, high), width, height, levels, then each
//   level's DXT1 blocks untiled, row after row
//
// The image's size and time are kept so a replaced image is decoded again. Only the
// levels above the packed mip tail are made (the smallest ones share one tile and
// can't be copied level by level); a case on screen never gets that small.
static const DWORD CACHE_HEADER = 7 * 4;
static const DWORD MAX_CACHE    = 16 * 1024 * 1024;

struct ArtStamp { DWORD dwSize, dwTimeLo, dwTimeHi; };

// Mip levels a cover gets: all of them down to the packed tail (0 = the image is that small)
static UINT ArtLevels( UINT w, UINT h )
{
    return XGGetMipTailBaseLevel( w, h, FALSE );
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
    DWORD dwSize = GetFileSize( hFile, NULL );
    BYTE* pData = NULL;
    if( dwSize != INVALID_FILE_SIZE && dwSize > CACHE_HEADER && dwSize <= MAX_CACHE )
    {
        pData = new BYTE[dwSize];
        DWORD dwRead = 0;
        if( !ReadFile( hFile, pData, dwSize, &dwRead, NULL ) || dwRead != dwSize )
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
    BOOL bOK = memcmp( pData, "OCV1", 4 ) == 0 &&
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
    memcpy( pData, "OCV1", 4 );
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

    // A half-written file (the console turned off) is the wrong size, so it's never used
    char szCacheDir[MAX_PATH];
    sprintf_s( szCacheDir, "%sCache", szDir );
    CreateDirectory( szCacheDir, NULL );
    HANDLE hFile = CreateFile( szCache, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL );
    if( hFile != INVALID_HANDLE_VALUE )
    {
        DWORD dwWritten;
        WriteFile( hFile, pData, dwSize, &dwWritten, NULL );
        CloseHandle( hFile );
    }
    delete[] pData;
}

// Box art for an item from the covers folder, compressed to DXT1 with mips (from the
// cache when it's there). A wide image (a box scan) wraps the case; a tall one is the front.
// ARCADE items only show their package's icon (Covers\Icons, saved by the scan), kept
// uncompressed: DXT1 smears an image that small.
static void LoadArt( D3DDevice* pDevice, int nShelf, int i )
{
    s_Art[nShelf][i] = ART_NONE;
    const ScanItem& it = Scan_GetItem( nShelf, i );
    if( !it.szTitleId[0] )
        return;
    if( nShelf == SHELF_ARCADE )
    {
        char szIcon[MAX_PATH];
        sprintf_s( szIcon, "game:\\Covers\\Icons\\%s.png", it.szTitleId );
        D3DTexture* pTex = NULL;
        if( SUCCEEDED( D3DXCreateTextureFromFileExA( pDevice, szIcon, D3DX_DEFAULT_NONPOW2, D3DX_DEFAULT_NONPOW2, D3DX_DEFAULT,
                                                     0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, D3DX_DEFAULT, D3DX_DEFAULT, 0, NULL, NULL, &pTex ) ) )
        {
            s_pArt[nShelf][i] = pTex;
            s_Art[nShelf][i] = ART_ICON;
        }
        return;
    }
    static const char* c_szExt[] = { "jpg", "png" };
    for( int d = 0; d < ARRAYSIZE( c_szCoverDirs ); d++ )
        for( int e = 0; e < ARRAYSIZE( c_szExt ); e++ )
        {
            char szPath[MAX_PATH];
            sprintf_s( szPath, "%s%s.%s", c_szCoverDirs[d], it.szTitleId, c_szExt[e] );
            WIN32_FILE_ATTRIBUTE_DATA Attr;
            if( !GetFileAttributesExA( szPath, GetFileExInfoStandard, &Attr ) )
                continue;
            const ArtStamp Stamp = { Attr.nFileSizeLow, Attr.ftLastWriteTime.dwLowDateTime,
                                     Attr.ftLastWriteTime.dwHighDateTime };
            char szCache[MAX_PATH];
            sprintf_s( szCache, "%sCache\\%s.dxt", c_szCoverDirs[d], it.szTitleId );

            D3DTexture* pTex = LoadCachedArt( pDevice, szCache, Stamp );
            if( !pTex )
            {
                // Levels down to the packed tail, so the result can be cached
                D3DXIMAGE_INFO Info;
                UINT nLevels = D3DX_DEFAULT;
                if( SUCCEEDED( D3DXGetImageInfoFromFileA( szPath, &Info ) ) && ArtLevels( Info.Width, Info.Height ) > 0 )
                    nLevels = ArtLevels( Info.Width, Info.Height );
                if( FAILED( D3DXCreateTextureFromFileExA( pDevice, szPath, D3DX_DEFAULT_NONPOW2, D3DX_DEFAULT_NONPOW2, nLevels,
                                                          0, D3DFMT_DXT1, D3DPOOL_MANAGED, D3DX_DEFAULT, D3DX_DEFAULT, 0, NULL, NULL, &pTex ) ) )
                    continue;
                SaveCachedArt( c_szCoverDirs[d], szCache, Stamp, pTex );
            }
            D3DSURFACE_DESC Desc;
            pTex->GetLevelDesc( 0, &Desc );
            s_pArt[nShelf][i] = pTex;
            s_Art[nShelf][i] = ( Desc.Width > Desc.Height * 1.2f ) ? ART_WRAP : ART_FRONT;
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

static BOOL s_bReleasePending = FALSE;   // the library changed: old covers go before new ones are made

// Makes what's missing for one item: its box art (if any), else its drawn cover and
// back. Returns the work done: 2 for an image file (decoding is the slow part), else
// 1 per drawn cover; stops early once nBudget is used.
static int EnsureCovers( D3DDevice* pDevice, int nShelf, int i, int nBudget )
{
    BYTE& art = s_Art[nShelf][i];
    if( art == ART_UNTRIED )
    {
        LoadArt( pDevice, nShelf, i );
        if( nShelf == s_nShelf && i == s_nIndex )
            s_bHudDirty = TRUE;   // X BACK COVER may now apply
        return 2;
    }
    int nMade = 0;
    const ScanItem& it = Scan_GetItem( nShelf, i );
    if( art == ART_NONE && !s_pCover[nShelf][i] && nMade < nBudget )
    {
        s_pCover[nShelf][i] = MakeCover( pDevice, it, FALSE );
        nMade++;
    }
    if( art != ART_WRAP && c_bShelfBacks[nShelf] && !s_pBack[nShelf][i] && nMade < nBudget )
    {
        s_pBack[nShelf][i] = MakeCover( pDevice, it, TRUE );
        nMade++;
    }
    return nMade;
}

int Library_CoversRemaining( int* pnTotal )
{
    int nTotal = 0, nLeft = 0;
    for( int s = 0; s < NUM_SHELVES; s++ )
        for( int i = 0; i < min( Scan_ItemCount( s ), MAX_SHELF ); i++ )
        {
            nTotal++;
            const BYTE art = s_Art[s][i];
            BOOL bDone = !s_bReleasePending && art != ART_UNTRIED &&
                         ( art != ART_NONE || s_pCover[s][i] ) &&
                         ( art == ART_WRAP || !c_bShelfBacks[s] || s_pBack[s][i] );
            if( !bDone )
                nLeft++;
        }
    if( pnTotal )
        *pnTotal = nTotal;
    return nLeft;
}

void Library_PreloadCovers( D3DDevice* pDevice )
{
    for( int s = 0; s < NUM_SHELVES; s++ )
        for( int i = 0; i < min( Scan_ItemCount( s ), MAX_SHELF ); i++ )
            while( EnsureCovers( pDevice, s, i, 2 ) )
                ;
}

void Library_PrepareCovers( D3DDevice* pDevice )
{
    if( s_bReleasePending )
    {
        // The GPU may still be drawing last frame's covers
        pDevice->BlockUntilIdle();
        ReleaseCovers();
        s_bReleasePending = FALSE;
    }

    // Two units of work a frame: the open shelf nearest the front first, then (after a
    // rescan replaced them) everything else in the background
    int nMade = 0;
    if( s_fLibScale > 0.01f || s_bOpen )
    {
        const int n = ItemCount();
        for( int d = 0; d <= 4 && nMade < 2; d++ )
            for( int sg = -1; sg <= 1 && nMade < 2; sg += 2 )
            {
                int i = s_nIndex + d * sg;
                if( i < 0 || i >= n || ( d == 0 && sg > 0 ) )
                    continue;
                nMade += EnsureCovers( pDevice, s_nShelf, i, 2 - nMade );
            }
    }
    for( int s = 0; s < NUM_SHELVES && nMade < 2; s++ )
        for( int i = 0; i < min( Scan_ItemCount( s ), MAX_SHELF ) && nMade < 2; i++ )
            nMade += EnsureCovers( pDevice, s, i, 2 - nMade );
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
    s_bReleasePending = TRUE;
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
                        s_bDeleting = TRUE;
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
    int   nOrder[MAX_SHELF], nItems = 0;
    float fDepth[MAX_SHELF];
    XMMATRIX matWorld[MAX_SHELF];
    for( int i = 0; i < n; i++ )
    {
        if( abs( i - s_nIndex ) > 4 )
            continue;
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
