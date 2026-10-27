// Orbit, an Xbox 360 homebrew launcher. Copyright (c) 2026 Noah Colbourne.
// Licensed under the GNU General Public License v3.0 or later; see LICENSE.

// Scan.cpp : see Scan.h.

#include "stdafx.h"
#include <stdlib.h>
#include <ctype.h>
#include "Scan.h"

static const int   MAX_ITEMS = 256;
static const char* PATHS_FILE   = "game:\\paths.txt";
static const char* LIBRARY_FILE = "game:\\library.txt";

// There are no default paths: every setup keeps its games somewhere different, so
// Orbit starts with none and they're added in SETTINGS (see the README).
static ScanPath s_Paths[MAX_SCAN_PATHS];
static int      s_nPaths = 0;

// The library the shelves show (main thread only) and the one a scan builds
static ScanItem s_Lib[MAX_ITEMS];
static int      s_nLib = 0;
static int      s_nShelf[NUM_SHELVES];
static int      s_nShelfIdx[NUM_SHELVES][MAX_ITEMS];

static ScanItem       s_Work[MAX_ITEMS];
static volatile int   s_nWork = 0;
static volatile BOOL  s_bRunning = FALSE;
static volatile BOOL  s_bFinished = FALSE;
static ScanPath       s_WorkPaths[MAX_SCAN_PATHS];
static int            s_nWorkPaths = 0;
static int            s_nWorkPath  = 0;   // the path being walked, for its per-shelf counts


//-------------------------------------------------------------------------------------
// Helpers
//-------------------------------------------------------------------------------------
static DWORD LE32( const BYTE* p ) { return p[0] | ( p[1] << 8 ) | ( p[2] << 16 ) | ( (DWORD)p[3] << 24 ); }
static DWORD BE32( const BYTE* p ) { return ( (DWORD)p[0] << 24 ) | ( p[1] << 16 ) | ( p[2] << 8 ) | p[3]; }

static void JoinPath( char* szOut, size_t nOut, const char* szPath, const char* szName )
{
    size_t n = strlen( szPath );
    if( n && szPath[n - 1] == '\\' )
        sprintf_s( szOut, nOut, "%s%s", szPath, szName );
    else
        sprintf_s( szOut, nOut, "%s\\%s", szPath, szName );
}

static const char* BaseName( const char* szPath )
{
    const char* p = szPath + strlen( szPath );
    while( p > szPath && p[-1] != '\\' && p[-1] != ':' ) p--;
    return p;
}

static void ParentOf( const char* szPath, char* szOut, size_t nOut )
{
    strcpy_s( szOut, nOut, szPath );
    char* p = strrchr( szOut, '\\' );
    if( p ) *p = 0;
}

static void DriveOf( const char* szPath, char* szOut, size_t nOut )
{
    size_t i = 0;
    for( ; szPath[i] && szPath[i] != ':' && i + 1 < nOut; i++ )
        szOut[i] = (char)toupper( (unsigned char)szPath[i] );
    szOut[i] = 0;
}

// UTF-16 (either byte order) to printable ASCII; other characters become '?'
static void WideToAscii( const BYTE* p, int nChars, BOOL bBigEndian, char* szOut, size_t nOut )
{
    size_t n = 0;
    for( int i = 0; i < nChars && n + 1 < nOut; i++ )
    {
        WORD c = bBigEndian ? (WORD)( ( p[i * 2] << 8 ) | p[i * 2 + 1] ) : (WORD)( p[i * 2] | ( p[i * 2 + 1] << 8 ) );
        if( c == 0 )
            break;
        szOut[n++] = ( c >= 32 && c < 127 ) ? (char)c : '?';
    }
    while( n && szOut[n - 1] == ' ' ) n--;
    szOut[n] = 0;
}

// Xbox 360 packed version: major.minor.build.qfe ("--" when not set, as in most packages)
static void FormatVersion360( DWORD v, char* sz, size_t n )
{
    if( v == 0 )
        strcpy_s( sz, n, "--" );
    else
        sprintf_s( sz, n, "%u.%u.%u.%u", ( v >> 28 ) & 0xF, ( v >> 24 ) & 0xF, ( v >> 8 ) & 0xFFFF, v & 0xFF );
}

static BOOL ReadHead( const char* szPath, BYTE* pBuf, DWORD dwSize, DWORD dwOffset = 0 )
{
    HANDLE h = CreateFileA( szPath, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL );
    if( h == INVALID_HANDLE_VALUE )
        return FALSE;
    DWORD dwRead = 0;
    BOOL bOK = ( SetFilePointer( h, dwOffset, NULL, FILE_BEGIN ) != INVALID_SET_FILE_POINTER ) &&
               ReadFile( h, pBuf, dwSize, &dwRead, NULL ) && dwRead == dwSize;
    CloseHandle( h );
    return bOK;
}

static ULONGLONG SizeOf( const char* szPath )
{
    WIN32_FIND_DATAA fd;
    char szFind[MAX_PATH], szChild[MAX_PATH];
    JoinPath( szFind, sizeof(szFind), szPath, "*" );
    HANDLE hFind = FindFirstFileA( szFind, &fd );
    if( hFind == INVALID_HANDLE_VALUE )
        return 0;
    ULONGLONG q = 0;
    do
    {
        if( strcmp( fd.cFileName, "." ) == 0 || strcmp( fd.cFileName, ".." ) == 0 )
            continue;
        if( fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY )
        {
            JoinPath( szChild, sizeof(szChild), szPath, fd.cFileName );
            q += SizeOf( szChild );
        }
        else
            q += ( (ULONGLONG)fd.nFileSizeHigh << 32 ) | fd.nFileSizeLow;
    }
    while( FindNextFileA( hFind, &fd ) );
    FindClose( hFind );
    return q;
}


//-------------------------------------------------------------------------------------
// What a folder or package is
//-------------------------------------------------------------------------------------
static ScanItem* NewItem( int nShelf, const char* szLaunch )
{
    // This path feeds that shelf, even if another path found the item first
    s_WorkPaths[s_nWorkPath].nFound[nShelf]++;
    for( int i = 0; i < s_nWork; i++ )
        if( _stricmp( s_Work[i].szLaunch, szLaunch ) == 0 )
            return NULL;   // found through two paths
    if( s_nWork >= MAX_ITEMS )
        return NULL;
    ScanItem* p = &s_Work[s_nWork];
    ZeroMemory( p, sizeof(*p) );
    p->nShelf = nShelf;
    strcpy_s( p->szLaunch, szLaunch );
    DriveOf( szLaunch, p->szDrive, sizeof(p->szDrive) );
    return p;
}

// Keeps the item unless its shelf already has the same thing: same title and same title
// ID (an app installed both as a folder and as a package shows once). Different games
// that happen to share a title both stay. Without a title ID to compare, the title decides.
static void Commit()
{
    const ScanItem& p = s_Work[s_nWork];
    for( int i = 0; i < s_nWork; i++ )
    {
        const ScanItem& q = s_Work[i];
        if( q.nShelf != p.nShelf || _stricmp( q.szTitle, p.szTitle ) != 0 )
            continue;
        if( !q.szTitleId[0] || !p.szTitleId[0] || _stricmp( q.szTitleId, p.szTitleId ) == 0 )
            return;
    }
    s_nWork++;
}

// Folder with default.xbe: the folder names it (certificate titles can be "CDX" or
// "SONICHEROES"); title ID and version from the XBE certificate
static void AddXbe( const char* szDir )
{
    char szXbe[MAX_PATH];
    JoinPath( szXbe, sizeof(szXbe), szDir, "default.xbe" );
    ScanItem* p = NewItem( SHELF_XBOX, szXbe );
    if( !p )
        return;
    strcpy_s( p->szTitle, BaseName( szDir ) );
    strcpy_s( p->szVersion, "--" );

    BYTE Head[0x200], Cert[0x1D0];
    if( ReadHead( szXbe, Head, sizeof(Head) ) && memcmp( Head, "XBEH", 4 ) == 0 )
    {
        DWORD dwBase = LE32( Head + 0x104 ), dwCert = LE32( Head + 0x118 );
        if( dwCert > dwBase && ReadHead( szXbe, Cert, sizeof(Cert), dwCert - dwBase ) )
        {
            sprintf_s( p->szVersion, "%u", LE32( Cert + 0xAC ) );
            sprintf_s( p->szTitleId, "%08X", LE32( Cert + 0x08 ) );
        }
    }
    strcpy_s( p->szDelete, szDir );
    p->qwSize = SizeOf( szDir );
    Commit();
}

// Folder with a .xex (default.xex, or for apps their own name, e.g. Aurora.xex):
// version from the XEX execution info; the folder names it
static void AddXex( const char* szDir, const char* szXexName, BOOL bApp )
{
    char szXex[MAX_PATH];
    JoinPath( szXex, sizeof(szXex), szDir, szXexName );

    ScanItem* p = NewItem( bApp ? SHELF_APPS : SHELF_360, szXex );
    if( !p )
        return;
    strcpy_s( p->szTitle, BaseName( szDir ) );
    strcpy_s( p->szVersion, "--" );

    BYTE Head[0x400];
    if( ReadHead( szXex, Head, sizeof(Head) ) && memcmp( Head, "XEX2", 4 ) == 0 )
    {
        DWORD dwCount = BE32( Head + 0x14 );
        for( DWORD k = 0; k < dwCount && 0x18 + k * 8 + 8 <= sizeof(Head); k++ )
        {
            DWORD dwKey = BE32( Head + 0x18 + k * 8 ), dwValue = BE32( Head + 0x18 + k * 8 + 4 );
            if( dwKey == 0x00040006 )   // execution info: media ID, version, base version, title ID, ...
            {
                BYTE Exec[16];
                if( ReadHead( szXex, Exec, sizeof(Exec), dwValue ) )
                {
                    FormatVersion360( BE32( Exec + 4 ), p->szVersion, sizeof(p->szVersion) );
                    if( BE32( Exec + 12 ) )
                        sprintf_s( p->szTitleId, "%08X", BE32( Exec + 12 ) );
                }
                break;
            }
        }
    }
    strcpy_s( p->szDelete, szDir );
    p->qwSize = SizeOf( szDir );
    Commit();
}

// A package's icon (the header's thumbnail, else its title thumbnail; PNGs, usually
// 64 x 64) saved once as game:\Covers\Icons\<title ID>.png for the ARCADE shelf. It's
// written under another name and renamed when whole, so a half-written file is never used.
static void SaveIcon( const char* szFile, const char* szTitleId )
{
    char szIcon[MAX_PATH], szTemp[MAX_PATH];
    sprintf_s( szIcon, "game:\\Covers\\Icons\\%s.png", szTitleId );
    WIN32_FILE_ATTRIBUTE_DATA Attr;
    if( GetFileAttributesExA( szIcon, GetFileExInfoStandard, &Attr ) )
        return;

    BYTE Sizes[8];
    if( !ReadHead( szFile, Sizes, sizeof(Sizes), 0x1712 ) )
        return;
    DWORD dwSize = BE32( Sizes ), dwOffset = 0x171A;
    if( dwSize == 0 )
        dwSize = BE32( Sizes + 4 ), dwOffset = 0x571A;
    if( dwSize == 0 || dwSize > 0x4000 )
        return;

    static const BYTE c_PngSig[8] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
    BYTE* pPng = new BYTE[dwSize];
    if( ReadHead( szFile, pPng, dwSize, dwOffset ) && dwSize > sizeof(c_PngSig) && memcmp( pPng, c_PngSig, sizeof(c_PngSig) ) == 0 )
    {
        CreateDirectoryA( "game:\\Covers", NULL );
        CreateDirectoryA( "game:\\Covers\\Icons", NULL );
        sprintf_s( szTemp, "game:\\Covers\\Icons\\%s.tmp", szTitleId );
        HANDLE h = CreateFileA( szTemp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL );
        if( h != INVALID_HANDLE_VALUE )
        {
            DWORD dwWritten = 0;
            BOOL bOK = WriteFile( h, pPng, dwSize, &dwWritten, NULL ) && dwWritten == dwSize;
            CloseHandle( h );
            if( !bOK || !MoveFileA( szTemp, szIcon ) )
                DeleteFileA( szTemp );
        }
    }
    delete[] pPng;
}

// A content package (STFS: LIVE / PIRS / CON): display name, type and version from its header
static void AddPackage( const char* szFile, ULONGLONG qwFileSize )
{
    BYTE Head[0x520];
    if( !ReadHead( szFile, Head, sizeof(Head) ) )
        return;
    if( memcmp( Head, "LIVE", 4 ) != 0 && memcmp( Head, "PIRS", 4 ) != 0 && memcmp( Head, "CON ", 4 ) != 0 )
        return;

    int nShelf;
    switch( BE32( Head + 0x344 ) )
    {
    case 0x00007000: nShelf = SHELF_360;    break;   // games on demand
    case 0x000D0000: nShelf = SHELF_ARCADE; break;   // arcade
    case 0x00080000: nShelf = SHELF_APPS;   break;   // demos / homebrew packages
    default:         return;                         // saves, add-ons, caches, ...
    }

    ScanItem* p = NewItem( nShelf, szFile );
    if( !p )
        return;
    WideToAscii( Head + 0x411, 128, TRUE, p->szTitle, sizeof(p->szTitle) );
    if( !p->szTitle[0] )
        strcpy_s( p->szTitle, BaseName( szFile ) );
    FormatVersion360( BE32( Head + 0x358 ), p->szVersion, sizeof(p->szVersion) );
    sprintf_s( p->szTitleId, "%08X", BE32( Head + 0x360 ) );
    if( nShelf == SHELF_ARCADE )
        SaveIcon( szFile, p->szTitleId );

    // The package and its "<name>.data" folder; delete takes the type folder holding them
    char szData[MAX_PATH];
    sprintf_s( szData, "%s.data", szFile );
    p->qwSize = qwFileSize + SizeOf( szData );
    ParentOf( szFile, p->szDelete, sizeof(p->szDelete) );
    Commit();
}

// Looks at a folder's own entries, then walks into its subfolders while depth lasts
static void Walk( const char* szDir, int nLeft )
{
    WIN32_FIND_DATAA fd;
    char szFind[MAX_PATH], szChild[MAX_PATH];
    JoinPath( szFind, sizeof(szFind), szDir, "*" );
    HANDLE hFind = FindFirstFileA( szFind, &fd );
    if( hFind == INVALID_HANDLE_VALUE )
        return;

    // Is this under an Apps folder? Apps launch from any .xex, not only default.xex
    char szLower[MAX_PATH];
    size_t i = 0;
    for( ; szDir[i] && i + 1 < sizeof(szLower); i++ ) szLower[i] = (char)tolower( (unsigned char)szDir[i] );
    szLower[i] = 0;
    BOOL bApp = strstr( szLower, "\\apps\\" ) != NULL;

    // A game or app folder ends the walk here. An app's .xex: default.xex, else one
    // named after the folder, else the last by name (the newest numbered build)
    char szFolderXex[MAX_PATH], szNamed[MAX_PATH] = "", szLast[MAX_PATH] = "";
    sprintf_s( szFolderXex, "%s.xex", BaseName( szDir ) );
    BOOL bXbe = FALSE, bXex = FALSE;
    do
    {
        if( fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY )
            continue;
        if( _stricmp( fd.cFileName, "default.xbe" ) == 0 ) bXbe = TRUE;
        if( _stricmp( fd.cFileName, "default.xex" ) == 0 ) bXex = TRUE;
        size_t n = strlen( fd.cFileName );
        if( bApp && n > 4 && _stricmp( fd.cFileName + n - 4, ".xex" ) == 0 )
        {
            if( _stricmp( fd.cFileName, szFolderXex ) == 0 )
                strcpy_s( szNamed, fd.cFileName );
            if( !szLast[0] || _stricmp( fd.cFileName, szLast ) > 0 )
                strcpy_s( szLast, fd.cFileName );
        }
    }
    while( FindNextFileA( hFind, &fd ) );
    FindClose( hFind );
    if( bXbe )      { AddXbe( szDir ); return; }
    if( bXex )      { AddXex( szDir, "default.xex", bApp ); return; }
    if( szNamed[0] ) { AddXex( szDir, szNamed, TRUE ); return; }
    if( szLast[0] )  { AddXex( szDir, szLast, TRUE ); return; }

    // Packages sit in a content type folder (..\<title ID>\<type>\<file>)
    const char* szHere = BaseName( szDir );
    BOOL bTypeFolder = strlen( szHere ) == 8 &&
                       ( _stricmp( szHere, "00007000" ) == 0 || _stricmp( szHere, "000D0000" ) == 0 || _stricmp( szHere, "00080000" ) == 0 );

    hFind = FindFirstFileA( szFind, &fd );
    if( hFind == INVALID_HANDLE_VALUE )
        return;
    do
    {
        if( strcmp( fd.cFileName, "." ) == 0 || strcmp( fd.cFileName, ".." ) == 0 )
            continue;
        JoinPath( szChild, sizeof(szChild), szDir, fd.cFileName );
        if( fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY )
        {
            // Skip a package's data folder; walk others while depth lasts
            size_t n = strlen( fd.cFileName );
            BOOL bData = n > 5 && _stricmp( fd.cFileName + n - 5, ".data" ) == 0;
            if( !bData && nLeft > 1 )
                Walk( szChild, nLeft - 1 );
        }
        else if( bTypeFolder )
        {
            AddPackage( szChild, ( (ULONGLONG)fd.nFileSizeHigh << 32 ) | fd.nFileSizeLow );
        }
    }
    while( FindNextFileA( hFind, &fd ) );
    FindClose( hFind );
}

static DWORD WINAPI ScanProc( LPVOID )
{
    for( s_nWorkPath = 0; s_nWorkPath < s_nWorkPaths; s_nWorkPath++ )
        Walk( s_WorkPaths[s_nWorkPath].szPath, s_WorkPaths[s_nWorkPath].nDepth );
    s_bFinished = TRUE;
    s_bRunning = FALSE;
    return 0;
}


//-------------------------------------------------------------------------------------
// Saved files
//-------------------------------------------------------------------------------------
static void SavePaths()
{
    FILE* f = NULL;
    if( fopen_s( &f, PATHS_FILE, "w" ) != 0 || !f )
        return;
    // depth, path, then whether it's been scanned and what it found per shelf
    for( int i = 0; i < s_nPaths; i++ )
    {
        const ScanPath& p = s_Paths[i];
        fprintf( f, "%d\t%s\t%d\t%d\t%d\t%d\t%d\n", p.nDepth, p.szPath, p.bScanned ? 1 : 0,
                 p.nFound[0], p.nFound[1], p.nFound[2], p.nFound[3] );
    }
    fclose( f );
}

static void LoadPaths()
{
    s_nPaths = 0;
    FILE* f = NULL;
    if( fopen_s( &f, PATHS_FILE, "r" ) == 0 && f )
    {
        char szLine[MAX_PATH + 16];
        while( s_nPaths < MAX_SCAN_PATHS && fgets( szLine, sizeof(szLine), f ) )
        {
            szLine[strcspn( szLine, "\r\n" )] = 0;
            char* pField[7];
            int n = 0;
            char* q = szLine;
            while( n < 7 )
            {
                pField[n++] = q;
                q = strchr( q, '\t' );
                if( !q ) break;
                *q++ = 0;
            }
            if( n < 2 )
                continue;
            ScanPath& p = s_Paths[s_nPaths++];
            ZeroMemory( &p, sizeof(p) );
            p.nDepth = max( MIN_DEPTH, min( MAX_DEPTH, atoi( pField[0] ) ) );
            strcpy_s( p.szPath, pField[1] );
            if( n >= 7 )
            {
                p.bScanned = atoi( pField[2] ) != 0;
                for( int s = 0; s < NUM_SHELVES; s++ )
                    p.nFound[s] = atoi( pField[3 + s] );
            }
        }
        fclose( f );
    }
    // No file yet (first run): no paths
}

static void SaveLibrary()
{
    FILE* f = NULL;
    if( fopen_s( &f, LIBRARY_FILE, "w" ) != 0 || !f )
        return;
    for( int i = 0; i < s_nLib; i++ )
    {
        const ScanItem& it = s_Lib[i];
        fprintf( f, "%d\t%s\t%s\t%I64u\t%s\t%s\t%s\t%s\n", it.nShelf, it.szTitle, it.szVersion, it.qwSize, it.szLaunch, it.szDelete, it.szDrive,
                 it.szTitleId[0] ? it.szTitleId : "-" );
    }
    fclose( f );
}

static int CompareTitles( const void* a, const void* b )
{
    return _stricmp( s_Lib[*(const int*)a].szTitle, s_Lib[*(const int*)b].szTitle );
}

// Groups the library by shelf, each alphabetical
static void IndexLibrary()
{
    for( int s = 0; s < NUM_SHELVES; s++ )
        s_nShelf[s] = 0;
    for( int i = 0; i < s_nLib; i++ )
    {
        int s = s_Lib[i].nShelf;
        if( s >= 0 && s < NUM_SHELVES )
            s_nShelfIdx[s][s_nShelf[s]++] = i;
    }
    for( int s = 0; s < NUM_SHELVES; s++ )
        qsort( s_nShelfIdx[s], s_nShelf[s], sizeof(int), CompareTitles );
}

static BOOL LoadLibrary()
{
    FILE* f = NULL;
    if( fopen_s( &f, LIBRARY_FILE, "r" ) != 0 || !f )
        return FALSE;
    s_nLib = 0;
    char szLine[1024];
    BOOL bOldFormat = FALSE;   // saved before title IDs were kept: rescan for them
    while( s_nLib < MAX_ITEMS && fgets( szLine, sizeof(szLine), f ) )
    {
        szLine[strcspn( szLine, "\r\n" )] = 0;
        char* pField[8];
        int n = 0;
        char* p = szLine;
        while( n < 8 )
        {
            pField[n++] = p;
            p = strchr( p, '\t' );
            if( !p ) break;
            *p++ = 0;
        }
        if( n < 8 )
        {
            bOldFormat = TRUE;
            continue;
        }
        ScanItem& it = s_Lib[s_nLib++];
        it.nShelf = atoi( pField[0] );
        strcpy_s( it.szTitle, pField[1] );
        strcpy_s( it.szVersion, pField[2] );
        it.qwSize = _strtoui64( pField[3], NULL, 10 );
        strcpy_s( it.szLaunch, pField[4] );
        strcpy_s( it.szDelete, pField[5] );
        strcpy_s( it.szDrive, pField[6] );
        strcpy_s( it.szTitleId, strcmp( pField[7], "-" ) ? pField[7] : "" );
    }
    fclose( f );
    IndexLibrary();
    return !bOldFormat;
}


//-------------------------------------------------------------------------------------
// Public
//-------------------------------------------------------------------------------------
void Scan_Init()
{
    LoadPaths();
    if( !LoadLibrary() )
        Scan_Start();
}

int Scan_PathCount() { return s_nPaths; }
const ScanPath& Scan_GetPath( int i ) { return s_Paths[i]; }

void Scan_SetDepth( int i, int nDepth )
{
    if( i < 0 || i >= s_nPaths )
        return;
    int nNew = max( MIN_DEPTH, min( MAX_DEPTH, nDepth ) );
    if( nNew == s_Paths[i].nDepth )
        return;
    s_Paths[i].nDepth = nNew;
    s_Paths[i].bScanned = FALSE;   // what it finds may change
    SavePaths();
}

void Scan_RemovePath( int i )
{
    if( i < 0 || i >= s_nPaths )
        return;
    for( int k = i; k < s_nPaths - 1; k++ )
        s_Paths[k] = s_Paths[k + 1];
    s_nPaths--;
    SavePaths();
}

BOOL Scan_AddPath( const char* szPath, int nDepth )
{
    if( s_nPaths >= MAX_SCAN_PATHS )
        return FALSE;
    char szClean[MAX_PATH];
    strcpy_s( szClean, szPath );
    size_t n = strlen( szClean );
    while( n > 1 && szClean[n - 1] == '\\' && szClean[n - 2] != ':' ) szClean[--n] = 0;
    for( int i = 0; i < s_nPaths; i++ )
        if( _stricmp( s_Paths[i].szPath, szClean ) == 0 )
            return FALSE;
    ZeroMemory( &s_Paths[s_nPaths], sizeof(ScanPath) );
    strcpy_s( s_Paths[s_nPaths].szPath, szClean );
    s_Paths[s_nPaths].nDepth = max( MIN_DEPTH, min( MAX_DEPTH, nDepth ) );
    s_nPaths++;
    SavePaths();
    return TRUE;
}

void Scan_ClearPaths()
{
    s_nPaths = 0;
    SavePaths();
}

void Scan_Start()
{
    if( s_bRunning )
        return;
    s_nWorkPaths = s_nPaths;
    for( int i = 0; i < s_nPaths; i++ )
    {
        s_WorkPaths[i] = s_Paths[i];
        for( int s = 0; s < NUM_SHELVES; s++ )
            s_WorkPaths[i].nFound[s] = 0;
    }
    s_nWork = 0;
    s_bFinished = FALSE;
    s_bRunning = TRUE;
    HANDLE h = CreateThread( NULL, 256 * 1024, ScanProc, NULL, 0, NULL );
    if( h )
        CloseHandle( h );
    else
        s_bRunning = FALSE;
}

BOOL Scan_IsRunning() { return s_bRunning; }
int  Scan_FoundSoFar() { return s_nWork; }

BOOL Scan_Poll()
{
    if( !s_bFinished )
        return FALSE;
    s_bFinished = FALSE;
    s_nLib = s_nWork;
    for( int i = 0; i < s_nLib; i++ )
        s_Lib[i] = s_Work[i];
    IndexLibrary();
    SaveLibrary();

    // Each path's results, matched by name and depth (paths edited during the scan stay unscanned)
    for( int i = 0; i < s_nPaths; i++ )
        for( int w = 0; w < s_nWorkPaths; w++ )
            if( _stricmp( s_Paths[i].szPath, s_WorkPaths[w].szPath ) == 0 && s_Paths[i].nDepth == s_WorkPaths[w].nDepth )
            {
                s_Paths[i].bScanned = TRUE;
                for( int s = 0; s < NUM_SHELVES; s++ )
                    s_Paths[i].nFound[s] = s_WorkPaths[w].nFound[s];
            }
    SavePaths();
    return TRUE;
}

int Scan_ItemCount( int nShelf ) { return ( nShelf >= 0 && nShelf < NUM_SHELVES ) ? s_nShelf[nShelf] : 0; }
const ScanItem& Scan_GetItem( int nShelf, int i ) { return s_Lib[s_nShelfIdx[nShelf][i]]; }
int Scan_TotalItems() { return s_nLib; }
