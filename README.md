# Quick Launch Menu

https://github.com/bytos/Quick.Launch.Menu

`qlm.exe` — popup of a folder or a Windows special folder. Icons come from a cache file so they appear together. One process; it exits when the menu closes.

## Command line

```
qlm.exe "D:\Data\Shortcuts"
qlm.exe 0x0002
qlm.exe 0
```

A complete number is a CSIDL. Anything else is a path.

## Cache (`%TEMP%`, on this PC `R:\Temp`)

| Argument | File |
|---|---|
| a path | `qlm-path.bin` |
| `0` / `0x0000` | `qlm-00.bin` |
| `0x0002` or `2` | `qlm-02.bin` |
| `0x0011` | `qlm-11.bin` |

Missing file: walk once, write the file, show the menu from memory.  
File present: read the file only. Do not walk the argument.

Delete the `.bin` to refresh (or reboot — RAM disk is gone).

## Menu

Click a shortcut to run it.  
A shortcut that points at a folder is a submenu (opened the first time you go into it, then saved).

A **path** (`D:\Data\Shortcuts`) lists **shortcuts only**. Real folders sitting in that directory are ignored.

A **CSIDL** (`0x0002`, …) lists what Windows puts there, including real subfolders (those are stored on the first run).

## Build (GitHub)

Workflow file is `.github/workflows/build.yml` (GitHub only runs it from that path). Upload that folder or use **Create new file** with that full name:

```yaml
name: Build

on:
  push:
    branches: [main, master]
  workflow_dispatch:

permissions:
  contents: write

jobs:
  build:
    runs-on: windows-latest
    steps:
      - uses: actions/checkout@v4
      - uses: microsoft/setup-msbuild@v2
      - run: msbuild qlm.sln /p:Configuration=Release /p:Platform=x64 /m
      - shell: pwsh
        run: |
          if (-not (Test-Path "x64\Release\qlm.exe")) {
            throw "x64\Release\qlm.exe was not produced"
          }
      - uses: softprops/action-gh-release@v2
        with:
          tag_name: latest
          name: qlm
          files: x64/Release/qlm.exe
          fail_on_unmatched_files: true
          make_latest: true
        env:
          GITHUB_TOKEN: ${{ secrets.GITHUB_TOKEN }}
```

Release asset is **`qlm.exe`**.
