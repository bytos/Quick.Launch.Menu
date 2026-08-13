#ifndef QLM_H
#define QLM_H

#include <windows.h>
#include <shlobj.h>

#define QLM_VERSION     L"1.0"

#define QLM_MAGIC       0x314D4C51  /* 'QLM1' */
#define QLM_VER         1

#define QF_FOLDER       0x0001
#define QF_LINK         0x0002
#define QF_FILESYSTEM   0x0004
#define QF_LAZY         0x0008

typedef struct QlmItem
{
	DWORD flags;
	WCHAR *name;
	WCHAR *path;
	LPITEMIDLIST pidl;
	HICON icon;
	HBITMAP bmp;
	struct QlmItem *kids;
	UINT nkids;
	UINT akids;
} QlmItem;

BOOL Cache_Open(BOOL isPath, int csidl, LPCWSTR pszPath);
void Cache_Close(void);
BOOL Cache_FillLazy(QlmItem *it);
QlmItem *Cache_Root(void);

#endif
