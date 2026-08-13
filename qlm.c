#include <windows.h>
#include <shlobj.h>
#include <shellapi.h>
#include "qlm.h"

#define ID_BASE 1
#define MAX_CMD 8192
#define WM_QLM_CLOSE (WM_APP + 1)
#define QLM_CLASS    L"QlmOwner"

static QlmItem *g_cmd[MAX_CMD];
static UINT g_ncmd;

struct MenuBind
{
	HMENU menu;
	QlmItem *it;
};
static struct MenuBind g_bind[2048];
static UINT g_nbind;

WCHAR *StrToDword(WCHAR *pszStr, DWORD *pdw);

static void BindMenu(HMENU menu, QlmItem *it)
{
	if(g_nbind < ARRAYSIZE(g_bind))
	{
		g_bind[g_nbind].menu = menu;
		g_bind[g_nbind].it = it;
		g_nbind++;
	}
}

static QlmItem *ItemFromMenu(HMENU menu)
{
	UINT i;
	for(i = 0; i < g_nbind; i++)
	{
		if(g_bind[i].menu == menu)
			return g_bind[i].it;
	}
	return NULL;
}

static UINT CmdOf(QlmItem *it)
{
	if(g_ncmd >= MAX_CMD)
		return 0;
	g_cmd[g_ncmd] = it;
	return ID_BASE + g_ncmd++;
}

static void SetIcon(HMENU menu, UINT pos, QlmItem *it)
{
	MENUITEMINFOW mii;
	if(!it || !it->bmp)
		return;
	ZeroMemory(&mii, sizeof(mii));
	mii.cbSize = sizeof(mii);
	mii.fMask = MIIM_BITMAP;
	mii.hbmpItem = it->bmp;
	SetMenuItemInfoW(menu, pos, TRUE, &mii);
}

static void FillMenu(HMENU menu, QlmItem *folder);

static void AppendItem(HMENU menu, QlmItem *it)
{
	UINT pos;
	if(!it || !it->name)
		return;
	pos = (UINT)GetMenuItemCount(menu);
	if(it->flags & QF_FOLDER)
	{
		HMENU sub = CreatePopupMenu();
		if(!sub)
			return;
		BindMenu(sub, it);
		if(!(it->flags & QF_LAZY))
			FillMenu(sub, it);
		AppendMenuW(menu, MF_POPUP, (UINT_PTR)sub, it->name);
	}
	else
	{
		UINT id = CmdOf(it);
		if(!id)
			return;
		AppendMenuW(menu, MF_STRING, id, it->name);
	}
	SetIcon(menu, pos, it);
}

static void FillMenu(HMENU menu, QlmItem *folder)
{
	UINT i;
	if(!menu || !folder)
		return;
	for(i = 0; i < folder->nkids; i++)
		AppendItem(menu, &folder->kids[i]);
	if(!folder->nkids)
		AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, L"(empty)");
}

static void Invoke(QlmItem *it)
{
	SHELLEXECUTEINFOW sei;
	if(!it)
		return;
	ZeroMemory(&sei, sizeof(sei));
	sei.cbSize = sizeof(sei);
	sei.nShow = SW_SHOWNORMAL;
	if(it->path && it->path[0])
	{
		sei.lpFile = it->path;
		ShellExecuteExW(&sei);
	}
	else if(it->pidl)
	{
		sei.fMask = SEE_MASK_IDLIST;
		sei.lpIDList = it->pidl;
		ShellExecuteExW(&sei);
	}
}

static void PumpABit(void)
{
	DWORD start = GetTickCount();
	MSG msg;
	while(GetTickCount() - start < 800)
	{
		DWORD st = MsgWaitForMultipleObjectsEx(0, NULL, 800 - (GetTickCount() - start),
			QS_ALLINPUT, MWMO_INPUTAVAILABLE);
		if(st == WAIT_TIMEOUT)
			break;
		while(PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE))
		{
			TranslateMessage(&msg);
			DispatchMessageW(&msg);
		}
	}
}

static BOOL CALLBACK CloseOther(HWND hwnd, LPARAM lp)
{
	WCHAR cls[32];
	UNREFERENCED_PARAMETER(lp);
	if(GetClassNameW(hwnd, cls, ARRAYSIZE(cls)) && !lstrcmpW(cls, QLM_CLASS))
		PostMessageW(hwnd, WM_QLM_CLOSE, 0, 0);
	return TRUE;
}

static void ClosePrevious(void)
{
	DWORD t = GetTickCount();
	EnumWindows(CloseOther, 0);
	while(GetTickCount() - t < 200)
	{
		if(!FindWindowW(QLM_CLASS, NULL))
			break;
		Sleep(10);
	}
}

static LRESULT CALLBACK OwnerProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	if(msg == WM_QLM_CLOSE)
	{
		EndMenu();
		return 0;
	}
	if(msg == WM_INITMENUPOPUP)
	{
		HMENU menu = (HMENU)wParam;
		QlmItem *it;
		if(GetMenuItemCount(menu) != 0)
			return 0;
		it = ItemFromMenu(menu);
		if(it && (it->flags & QF_FOLDER))
		{
			if(it->flags & QF_LAZY)
				Cache_FillLazy(it);
			FillMenu(menu, it);
		}
		return 0;
	}
	return DefWindowProcW(hwnd, msg, wParam, lParam);
}

static HWND MakeOwner(void)
{
	WNDCLASSEXW wc;
	ZeroMemory(&wc, sizeof(wc));
	wc.cbSize = sizeof(wc);
	wc.lpfnWndProc = OwnerProc;
	wc.hInstance = GetModuleHandleW(NULL);
	wc.lpszClassName = QLM_CLASS;
	RegisterClassExW(&wc);
	return CreateWindowExW(0, QLM_CLASS, L"", WS_POPUP,
		0, 0, 0, 0, NULL, NULL, wc.hInstance, NULL);
}

int main(void)
{
	int argc;
	WCHAR **argv;
	WCHAR *pPath;
	int csild;
	BOOL isPath;

	argv = CommandLineToArgvW(GetCommandLineW(), &argc);
	if(!argv)
		ExitProcess(0);

	if(argc < 2)
	{
		MessageBoxW(
			NULL,
			L"Usage:\n"
			L"qlm.exe folder\n"
			L"\n"
			L"The folder can be a path or a CSIDL identifier.\n"
			L"\n"
			L"Examples:\n"
			L"qlm.exe C:\\\n"
			L"qlm.exe \"D:\\Data\\Shortcuts\"\n"
			L"qlm.exe 0x0002",
			L"Quick Launch v" QLM_VERSION,
			MB_ICONASTERISK);
		LocalFree(argv);
		ExitProcess(0);
	}

	if(*StrToDword(argv[1], (DWORD *)&csild) != L'\0')
	{
		csild = 0;
		pPath = argv[1];
		isPath = TRUE;
	}
	else
	{
		pPath = NULL;
		isPath = FALSE;
	}

	if(SUCCEEDED(OleInitialize(NULL)))
	{
		if(Cache_Open(isPath, csild, pPath))
		{
			HWND hwnd;
			HMENU menu;
			POINT pt;
			UINT id = 0;

			hwnd = MakeOwner();
			menu = CreatePopupMenu();
			if(menu && hwnd)
			{
				QlmItem *root = Cache_Root();
				UINT i;
				for(i = 0; i < root->nkids; i++)
					AppendItem(menu, &root->kids[i]);
				if(root->nkids == 0)
					AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, L"(empty)");

				GetCursorPos(&pt);
				SetForegroundWindow(hwnd);
				id = (UINT)TrackPopupMenuEx(
					menu,
					TPM_RETURNCMD | TPM_LEFTALIGN | TPM_BOTTOMALIGN | TPM_RIGHTBUTTON,
					pt.x, pt.y, hwnd, NULL);
				PostMessageW(hwnd, WM_NULL, 0, 0);
				if(id >= ID_BASE && id - ID_BASE < g_ncmd)
					Invoke(g_cmd[id - ID_BASE]);
				DestroyMenu(menu);
			}
			if(hwnd)
				DestroyWindow(hwnd);
			if(id >= ID_BASE)
				PumpABit();
			Cache_Close();
		}
		else
		{
			MessageBoxW(NULL, L"Could not open the folder or the cache.",
				L"Quick Launch", MB_ICONERROR);
		}
		OleUninitialize();
	}

	LocalFree(argv);
	ExitProcess(0);
}

WCHAR *StrToDword(WCHAR *pszStr, DWORD *pdw)
{
	BOOL bMinus;
	DWORD dw, dw2;

	if(*pszStr == L'-')
	{
		bMinus = TRUE;
		pszStr++;
	}
	else
		bMinus = FALSE;

	dw = 0;

	if(pszStr[0] == L'0' && (pszStr[1] == L'x' || pszStr[1] == L'X'))
	{
		pszStr += 2;
		while(*pszStr != L'\0')
		{
			if(*pszStr >= L'0' && *pszStr <= L'9')
				dw2 = *pszStr - L'0';
			else if(*pszStr >= L'a' && *pszStr <= L'f')
				dw2 = *pszStr - L'a' + 0x0A;
			else if(*pszStr >= L'A' && *pszStr <= L'F')
				dw2 = *pszStr - L'A' + 0x0A;
			else
				break;
			dw <<= 0x04;
			dw |= dw2;
			pszStr++;
		}
	}
	else
	{
		while(*pszStr != L'\0')
		{
			if(*pszStr >= L'0' && *pszStr <= L'9')
				dw2 = *pszStr - L'0';
			else
				break;
			dw *= 10;
			dw += dw2;
			pszStr++;
		}
	}

	if(bMinus)
		*pdw = (DWORD)-(long)dw;
	else
		*pdw = dw;

	return pszStr;
}
