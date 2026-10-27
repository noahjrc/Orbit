// Orbit, an Xbox 360 homebrew launcher. Copyright (c) 2026 Noah Colbourne.
// Licensed under the GNU General Public License v3.0 or later; see LICENSE.

// Scan.h : finds the games and apps for the shelves by walking the content paths,
// like Aurora's Manage Paths. Each path is walked to its depth (1 = only the folder's
// own entries) and what turns up is sorted by what it is, not by where it was found:
//
//   folder with default.xbe                    -> XBOX      (title / version from the XBE)
//   folder with default.xex                    -> APPS if under an Apps folder, else XBOX 360
//   folder under Apps with another .xex        -> APPS      (e.g. Aurora\Aurora.xex)
//   package in a ...\00007000\ folder (GOD)    -> XBOX 360  (name / version from its header)
//   package in a ...\000D0000\ folder          -> ARCADE    (its icon saved to Covers\Icons)
//   package in a ...\00080000\ folder          -> APPS
//
// The paths live in game:\paths.txt and the last scan's results in game:\library.txt,
// so the shelves fill at once on start; a scan runs on start only when there's no
// saved library. Orbit starts with no paths. Scans run on a worker thread.

#pragma once

enum { SHELF_XBOX, SHELF_360, SHELF_ARCADE, SHELF_APPS, NUM_SHELVES };

struct ScanItem
{
    int       nShelf;
    char      szTitle[64];
    char      szVersion[24];
    ULONGLONG qwSize;
    char      szLaunch[MAX_PATH];   // what A launches
    char      szDelete[MAX_PATH];   // what delete removes (the game's folder, or its package folder)
    char      szDrive[8];           // "HDD1"
    char      szTitleId[12];        // "4D5307E6"; names its box art in the covers folder ("" if unknown)
};

struct ScanPath
{
    char szPath[MAX_PATH];
    int  nDepth;
    BOOL bScanned;                 // FALSE until a scan has walked it at this depth
    int  nFound[NUM_SHELVES];      // what the last scan found here, per shelf (an item
                                   // reachable from two paths counts for both)
};
static const int MAX_SCAN_PATHS = 12;
static const int MIN_DEPTH = 1, MAX_DEPTH = 6;

void  Scan_Init();

int   Scan_PathCount();
const ScanPath& Scan_GetPath( int i );
void  Scan_SetDepth( int i, int nDepth );
void  Scan_RemovePath( int i );
BOOL  Scan_AddPath( const char* szPath, int nDepth );   // FALSE if full or already there
void  Scan_ClearPaths();                                // removes every path

void  Scan_Start();
BOOL  Scan_IsRunning();
int   Scan_FoundSoFar();
// TRUE once each time a finished scan has replaced the library
BOOL  Scan_Poll();

int   Scan_ItemCount( int nShelf );
const ScanItem& Scan_GetItem( int nShelf, int i );
int   Scan_TotalItems();
