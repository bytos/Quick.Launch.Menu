/* Path → %TEMP%\qlm-path.bin. Number → %TEMP%\qlm-XX.bin (low byte, hex).
   Missing file: walk once, write, keep the tree in memory.
   File present: read it. Do not walk the argument.
   A path listing is .lnk files only. Everything else is ignored.
   A .lnk that points at a folder is a submenu (also .lnk only).
   A CSIDL still lists what Windows puts there. */

#include <windows.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <stdlib.h>
#include "qlm.h"

#define MAX_DEPTH 16
#define LAZY_KIDS  0xFFFFFFFFu

static WCHAR g_szFile[MAX_PATH];
static QlmItem g_root;

static BOOL Wr(HANDLE h, const void *p, DWORD cb)
{
	DWORD w = 0;
	return cb == 0 || (WriteFile(h, p, cb, &w, NULL) && w == cb);
}
static BOOL Rd(HANDLE h, void *p, DWORD cb)
{
	DWORD r = 0;
	return cb == 0 || (ReadFile(h, p, cb, &r, NULL) && r == cb);
}
static BOOL WrDw(HANDLE h, DWORD v) { return Wr(h, &v, sizeof(v)); }
static BOOL RdDw(HANDLE h, DWORD *v) { return Rd(h, v, sizeof(*v)); }

static BOOL WrStr(HANDLE h, LPCWSTR s)
{
	DWORD cch = s ? (DWORD)lstrlenW(s) : 0;
	return WrDw(h, cch) && Wr(h, s, cch * sizeof(WCHAR));
}

static WCHAR *RdStr(HANDLE h)
{
	DWORD cch;
	WCHAR *s;
	if(!RdDw(h, &cch) || cch > 4096)
		return NULL;
	s = (WCHAR *)CoTaskMemAlloc((cch + 1) * sizeof(WCHAR));
	if(!s)
		return NULL;
	if(cch && !Rd(h, s, cch * sizeof(WCHAR)))
	{
		CoTaskMemFree(s);
		return NULL;
	}
	s[cch] = 0;
	return s;
}

static BOOL WrBlk(HANDLE h, const void *p, DWORD cb)
{
	return WrDw(h, cb) && Wr(h, p, cb);
}

static void *RdBlk(HANDLE h, DWORD *pcb)
{
	DWORD cb;
	void *p;
	if(!RdDw(h, &cb) || cb > 256 * 1024)
		return NULL;
	*pcb = cb;
	if(cb == 0)
		return NULL;
	p = CoTaskMemAlloc(cb);
	if(!p || !Rd(h, p, cb))
	{
		if(p) CoTaskMemFree(p);
		return NULL;
	}
	return p;
}

static HBITMAP IconToBmp(HICON hIcon)
{
	int cx, cy;
	BITMAPINFO bmi;
	void *bits;
	HDC hdc, mem;
	HBITMAP hbmp, old;
	RECT rc;

	if(!hIcon)
		return NULL;
	cx = GetSystemMetrics(SM_CXSMICON);
	cy = GetSystemMetrics(SM_CYSMICON);
	if(cx <= 0) cx = 16;
	if(cy <= 0) cy = 16;

	ZeroMemory(&bmi, sizeof(bmi));
	bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
	bmi.bmiHeader.biWidth = cx;
	bmi.bmiHeader.biHeight = -cy;
	bmi.bmiHeader.biPlanes = 1;
	bmi.bmiHeader.biBitCount = 32;
	bmi.bmiHeader.biCompression = BI_RGB;

	hdc = GetDC(NULL);
	hbmp = CreateDIBSection(hdc, &bmi, DIB_RGB_COLORS, &bits, NULL, 0);
	if(!hbmp)
	{
		ReleaseDC(NULL, hdc);
		return NULL;
	}
	if(bits)
		ZeroMemory(bits, (SIZE_T)cx * (SIZE_T)cy * 4);
	mem = CreateCompatibleDC(hdc);
	old = (HBITMAP)SelectObject(mem, hbmp);
	rc.left = rc.top = 0;
	rc.right = cx;
	rc.bottom = cy;
	DrawIconEx(mem, 0, 0, hIcon, cx, cy, 0, NULL, DI_NORMAL);
	SelectObject(mem, old);
	DeleteDC(mem);
	ReleaseDC(NULL, hdc);
	return hbmp;
}

static BOOL WrIcon(HANDLE h, HICON hIcon)
{
	ICONINFO ii;
	BITMAP bc, bm;
	DWORD cbC = 0, cbM = 0;
	void *pC = NULL, *pM = NULL;
	BOOL ok;

	ZeroMemory(&ii, sizeof(ii));
	ZeroMemory(&bc, sizeof(bc));
	ZeroMemory(&bm, sizeof(bm));
	if(hIcon && GetIconInfo(hIcon, &ii))
	{
		if(ii.hbmColor && GetObjectW(ii.hbmColor, sizeof(bc), &bc))
		{
			cbC = (DWORD)(bc.bmWidthBytes * bc.bmHeight);
			pC = HeapAlloc(GetProcessHeap(), 0, cbC);
			if(pC)
				GetBitmapBits(ii.hbmColor, cbC, pC);
		}
		if(ii.hbmMask && GetObjectW(ii.hbmMask, sizeof(bm), &bm))
		{
			cbM = (DWORD)(bm.bmWidthBytes * bm.bmHeight);
			pM = HeapAlloc(GetProcessHeap(), 0, cbM);
			if(pM)
				GetBitmapBits(ii.hbmMask, cbM, pM);
		}
		if(ii.hbmColor) DeleteObject(ii.hbmColor);
		if(ii.hbmMask) DeleteObject(ii.hbmMask);
	}
	ok = Wr(h, &bc, sizeof(bc)) && WrBlk(h, pC, pC ? cbC : 0) &&
		Wr(h, &bm, sizeof(bm)) && WrBlk(h, pM, pM ? cbM : 0);
	if(pC) HeapFree(GetProcessHeap(), 0, pC);
	if(pM) HeapFree(GetProcessHeap(), 0, pM);
	return ok;
}

static HICON RdIcon(HANDLE h)
{
	BITMAP bc, bm;
	DWORD cbC, cbM;
	void *pC, *pM;
	ICONINFO ii;
	HICON hIcon = NULL;

	ZeroMemory(&bc, sizeof(bc));
	ZeroMemory(&bm, sizeof(bm));
	if(!Rd(h, &bc, sizeof(bc)))
		return NULL;
	pC = RdBlk(h, &cbC);
	if(!Rd(h, &bm, sizeof(bm)))
	{
		if(pC) CoTaskMemFree(pC);
		return NULL;
	}
	pM = RdBlk(h, &cbM);

	ZeroMemory(&ii, sizeof(ii));
	ii.fIcon = TRUE;
	if(pC && cbC)
	{
		ii.hbmColor = CreateBitmap(bc.bmWidth, bc.bmHeight, bc.bmPlanes, bc.bmBitsPixel, NULL);
		if(ii.hbmColor)
			SetBitmapBits(ii.hbmColor, cbC, pC);
	}
	if(pM && cbM)
	{
		ii.hbmMask = CreateBitmap(bm.bmWidth, bm.bmHeight, bm.bmPlanes, bm.bmBitsPixel, NULL);
		if(ii.hbmMask)
			SetBitmapBits(ii.hbmMask, cbM, pM);
	}
	if(ii.hbmColor || ii.hbmMask)
		hIcon = CreateIconIndirect(&ii);
	if(ii.hbmColor) DeleteObject(ii.hbmColor);
	if(ii.hbmMask) DeleteObject(ii.hbmMask);
	if(pC) CoTaskMemFree(pC);
	if(pM) CoTaskMemFree(pM);
	return hIcon;
}

static void FreeItem(QlmItem *it)
{
	UINT i;
	if(!it)
		return;
	if(it->name) CoTaskMemFree(it->name);
	if(it->path) CoTaskMemFree(it->path);
	if(it->pidl) ILFree(it->pidl);
	if(it->icon) DestroyIcon(it->icon);
	if(it->bmp) DeleteObject(it->bmp);
	for(i = 0; i < it->nkids; i++)
		FreeItem(&it->kids[i]);
	if(it->kids)
		HeapFree(GetProcessHeap(), 0, it->kids);
	ZeroMemory(it, sizeof(*it));
}

static QlmItem *AddKid(QlmItem *parent)
{
	QlmItem *p;
	if(parent->nkids >= parent->akids)
	{
		UINT n = parent->akids ? parent->akids * 2 : 16;
		if(!parent->kids)
			p = (QlmItem *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, n * sizeof(QlmItem));
		else
			p = (QlmItem *)HeapReAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, parent->kids, n * sizeof(QlmItem));
		if(!p)
			return NULL;
		parent->kids = p;
		parent->akids = n;
	}
	p = &parent->kids[parent->nkids++];
	ZeroMemory(p, sizeof(*p));
	return p;
}

static int CmpItem(const void *a, const void *b)
{
	const QlmItem *ia = (const QlmItem *)a, *ib = (const QlmItem *)b;
	int fa = (ia->flags & QF_FOLDER) ? 0 : 1;
	int fb = (ib->flags & QF_FOLDER) ? 0 : 1;
	if(fa != fb)
		return fa - fb;
	return lstrcmpiW(ia->name ? ia->name : L"", ib->name ? ib->name : L"");
}

static void SortKids(QlmItem *it)
{
	if(it->nkids > 1)
		qsort(it->kids, it->nkids, sizeof(QlmItem), CmpItem);
}

static IShellFolder *FolderFromPidl(LPITEMIDLIST pidl)
{
	IShellFolder *desk = NULL, *psf = NULL;
	if(FAILED(SHGetDesktopFolder(&desk)))
		return NULL;
	if(!pidl || ILIsEmpty(pidl))
		return desk;
	if(FAILED(desk->BindToObject(pidl, NULL, IID_IShellFolder, (void **)&psf)) || !psf)
	{
		psf = desk;
		psf->AddRef();
	}
	desk->Release();
	return psf;
}

static WCHAR *DupStr(LPCWSTR s)
{
	UINT cch;
	WCHAR *d;
	if(!s)
		return NULL;
	cch = (UINT)lstrlenW(s) + 1;
	d = (WCHAR *)CoTaskMemAlloc(cch * sizeof(WCHAR));
	if(d)
		lstrcpyW(d, s);
	return d;
}

static BOOL WalkFolder(IShellFolder *psf, LPITEMIDLIST pidlFolder,
	QlmItem *parent, int depth, BOOL recurseFs, BOOL shortcutsOnly)
{
	IEnumIDList *penum = NULL;
	LPITEMIDLIST child;
	HRESULT hr;

	if(!psf || !parent || depth > MAX_DEPTH)
		return FALSE;

	hr = psf->EnumObjects(NULL, SHCONTF_FOLDERS | SHCONTF_NONFOLDERS, &penum);
	if(FAILED(hr) || !penum)
		return TRUE;

	while(penum->Next(1, &child, NULL) == S_OK)
	{
		QlmItem *it;
		SFGAOF attrs;
		STRRET sr;
		WCHAR buf[2084];
		LPITEMIDLIST abs;
		SHFILEINFOW sfi;

		attrs = SFGAO_FOLDER | SFGAO_LINK | SFGAO_FILESYSTEM | SFGAO_HASSUBFOLDER;
		psf->GetAttributesOf(1, (LPCITEMIDLIST *)&child, &attrs);

		ZeroMemory(&sr, sizeof(sr));
		buf[0] = 0;
		if(SUCCEEDED(psf->GetDisplayNameOf(child, SHGDN_FORPARSING, &sr)))
			StrRetToBufW(&sr, child, buf, ARRAYSIZE(buf));

		/* Path mode: real folders (travel) and .lnk (run). Ignore the rest. */
		if(shortcutsOnly)
		{
			BOOL isLnk = FALSE;
			BOOL isRealFolder = (attrs & SFGAO_FOLDER) && !(attrs & SFGAO_LINK);
			UINT cch = (UINT)lstrlenW(buf);
			if(cch >= 4 && lstrcmpiW(buf + cch - 4, L".lnk") == 0)
				isLnk = TRUE;
			if(!isLnk && !isRealFolder)
			{
				ILFree(child);
				continue;
			}
		}

		it = AddKid(parent);
		if(!it)
		{
			ILFree(child);
			break;
		}
		if(attrs & SFGAO_FOLDER)     it->flags |= QF_FOLDER;
		if(attrs & SFGAO_LINK)       it->flags |= QF_LINK;
		if(attrs & SFGAO_FILESYSTEM) it->flags |= QF_FILESYSTEM;
		if(buf[0])
			it->path = DupStr(buf);

		abs = ILCombine(pidlFolder, child);
		it->pidl = abs ? abs : ILClone(child);

		ZeroMemory(&sfi, sizeof(sfi));
		if(it->pidl && SHGetFileInfoW((LPCWSTR)it->pidl, 0, &sfi, sizeof(sfi),
			SHGFI_PIDL | SHGFI_ICON | SHGFI_SMALLICON | SHGFI_DISPLAYNAME))
		{
			it->icon = sfi.hIcon;
			if(sfi.szDisplayName[0])
				it->name = DupStr(sfi.szDisplayName);
		}
		if(!it->name)
		{
			ZeroMemory(&sr, sizeof(sr));
			if(SUCCEEDED(psf->GetDisplayNameOf(child, SHGDN_NORMAL, &sr)) &&
				SUCCEEDED(StrRetToBufW(&sr, child, buf, ARRAYSIZE(buf))))
				it->name = DupStr(buf);
			if(!it->name)
				it->name = DupStr(L"?");
		}
		if(!it->path)
		{
			ZeroMemory(&sr, sizeof(sr));
			if(SUCCEEDED(psf->GetDisplayNameOf(child, SHGDN_FORPARSING, &sr)) &&
				SUCCEEDED(StrRetToBufW(&sr, child, buf, ARRAYSIZE(buf))))
				it->path = DupStr(buf);
		}

		it->bmp = IconToBmp(it->icon);

		if((it->flags & QF_FOLDER) &&
			!(it->flags & QF_LINK) &&
			(it->flags & QF_FILESYSTEM) &&
			recurseFs && depth < MAX_DEPTH)
		{
			IShellFolder *sub = NULL;
			if(SUCCEEDED(psf->BindToObject(child, NULL, IID_IShellFolder, (void **)&sub)) && sub)
			{
				WalkFolder(sub, it->pidl, it, depth + 1, TRUE, FALSE);
				sub->Release();
			}
			else
				it->flags |= QF_LAZY;
		}
		else if(it->flags & QF_FOLDER)
			it->flags |= QF_LAZY;

		ILFree(child);
	}
	penum->Release();
	SortKids(parent);
	return TRUE;
}

static BOOL WalkRoot(BOOL isPath, int csidl, LPCWSTR pszPath)
{
	IShellFolder *desk = NULL, *psf = NULL;
	LPITEMIDLIST pidl = NULL;
	BOOL ok = FALSE;

	if(FAILED(SHGetDesktopFolder(&desk)))
		return FALSE;

	if(isPath)
	{
		if(FAILED(SHParseDisplayName(pszPath, NULL, &pidl, 0, NULL)) || !pidl)
		{
			desk->Release();
			return FALSE;
		}
	}
	else
	{
		if(FAILED(SHGetSpecialFolderLocation(NULL, csidl, &pidl)) || !pidl)
		{
			desk->Release();
			return FALSE;
		}
	}

	if(FAILED(desk->BindToObject(pidl, NULL, IID_IShellFolder, (void **)&psf)) || !psf)
	{
		psf = desk;
		psf->AddRef();
	}
	ok = WalkFolder(psf, pidl, &g_root, 0, TRUE, isPath);
	psf->Release();
	desk->Release();
	ILFree(pidl);
	return ok;
}

static BOOL SaveItem(HANDLE h, const QlmItem *it)
{
	UINT i;
	DWORD n;
	if(!WrDw(h, it->flags) ||
		!WrStr(h, it->name) ||
		!WrStr(h, it->path) ||
		!WrBlk(h, it->pidl, it->pidl ? ILGetSize(it->pidl) : 0) ||
		!WrIcon(h, it->icon))
		return FALSE;
	if(it->flags & QF_LAZY)
		n = LAZY_KIDS;
	else
		n = it->nkids;
	if(!WrDw(h, n))
		return FALSE;
	if(n != LAZY_KIDS)
	{
		for(i = 0; i < it->nkids; i++)
		{
			if(!SaveItem(h, &it->kids[i]))
				return FALSE;
		}
	}
	return TRUE;
}

static BOOL LoadItem(HANDLE h, QlmItem *it)
{
	DWORD cb, n, i;
	void *blk;

	ZeroMemory(it, sizeof(*it));
	if(!RdDw(h, &it->flags))
		return FALSE;
	it->name = RdStr(h);
	it->path = RdStr(h);
	blk = RdBlk(h, &cb);
	if(blk && cb >= 2)
		it->pidl = (LPITEMIDLIST)blk;
	else if(blk)
		CoTaskMemFree(blk);
	it->icon = RdIcon(h);
	it->bmp = IconToBmp(it->icon);
	if(!RdDw(h, &n))
		return FALSE;
	if(n == LAZY_KIDS)
	{
		it->flags |= QF_LAZY;
		return TRUE;
	}
	it->flags &= ~QF_LAZY;
	for(i = 0; i < n; i++)
	{
		QlmItem *kid = AddKid(it);
		if(!kid || !LoadItem(h, kid))
			return FALSE;
	}
	return TRUE;
}

static BOOL Save(void)
{
	HANDLE h;
	UINT i;
	if(!g_szFile[0])
		return FALSE;
	h = CreateFileW(g_szFile, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	if(h == INVALID_HANDLE_VALUE)
		return FALSE;
	if(!WrDw(h, QLM_MAGIC) || !WrDw(h, QLM_VER) || !WrDw(h, g_root.nkids))
	{
		CloseHandle(h);
		DeleteFileW(g_szFile);
		return FALSE;
	}
	for(i = 0; i < g_root.nkids; i++)
	{
		if(!SaveItem(h, &g_root.kids[i]))
		{
			CloseHandle(h);
			DeleteFileW(g_szFile);
			return FALSE;
		}
	}
	CloseHandle(h);
	return TRUE;
}

static BOOL Load(void)
{
	HANDLE h;
	DWORD magic, ver, count, i;

	h = CreateFileW(g_szFile, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if(h == INVALID_HANDLE_VALUE)
		return FALSE;
	if(!RdDw(h, &magic) || magic != QLM_MAGIC ||
		!RdDw(h, &ver) || ver != QLM_VER ||
		!RdDw(h, &count) || count > 100000)
	{
		CloseHandle(h);
		return FALSE;
	}
	for(i = 0; i < count; i++)
	{
		QlmItem *it = AddKid(&g_root);
		if(!it || !LoadItem(h, it))
		{
			CloseHandle(h);
			return FALSE;
		}
	}
	CloseHandle(h);
	return TRUE;
}

static BOOL MakeFileName(BOOL isPath, int csidl)
{
	DWORD n = (DWORD)lstrlenW(lstrcpyW(g_szFile, L"D:\\Data\\QLM\\"));
	WCHAR extra[32];
	if(n == 0 || n >= ARRAYSIZE(g_szFile))
		return FALSE;
	if(isPath)
		lstrcpyW(extra, L"qlm-path.bin");
	else
		wsprintfW(extra, L"qlm-%02x.bin", csidl & 0xFF);
	if((UINT)lstrlenW(g_szFile) + (UINT)lstrlenW(extra) >= ARRAYSIZE(g_szFile))
		return FALSE;
	lstrcatW(g_szFile, extra);
	return TRUE;
}

BOOL Cache_Open(BOOL isPath, int csidl, LPCWSTR pszPath)
{
	Cache_Close();
	if(!MakeFileName(isPath, csidl))
		return FALSE;
	if(GetFileAttributesW(g_szFile) != INVALID_FILE_ATTRIBUTES)
	{
		if(Load())
			return TRUE;
		FreeItem(&g_root);
		ZeroMemory(&g_root, sizeof(g_root));
	}
	if(!WalkRoot(isPath, csidl, pszPath))
		return FALSE;
	Save();
	return TRUE;
}

void Cache_Close(void)
{
	FreeItem(&g_root);
	g_szFile[0] = 0;
}

BOOL Cache_FillLazy(QlmItem *it)
{
	IShellFolder *psf;
	if(!it || !(it->flags & QF_FOLDER))
		return FALSE;
	if(!(it->flags & QF_LAZY))
		return TRUE;
	psf = FolderFromPidl(it->pidl);
	if(!psf)
		return FALSE;
	WalkFolder(psf, it->pidl, it, 0, FALSE, TRUE);
	psf->Release();
	it->flags &= ~QF_LAZY;
	Save();
	return TRUE;
}

QlmItem *Cache_Root(void)
{
	return &g_root;
}
