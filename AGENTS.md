# CornerstoneDesigner — Agent Guide

Language

始终使用中文进行交互。

## File Encoding

所有源代码文件（.h/.cpp）必须保存为 **UTF-8 with BOM** 编码格式。不要使用 UTF-8 without BOM 或其他编码。

## Project

C++17 UI designer tool for [UICornerstone](https://github.com/SeaOceanLiu/UICornerstone) (SDL3 backend). Dogfooding — the designer itself is a UICornerstone app. GPL v3.

## State

Skeleton only — one initial commit with `README.md`, `LICENSE`, and `.gitignore` (standard Visual Studio template). No source code yet.

## Repo Layout

- `doc/` — waterfall design documents (需求 -> 设计 -> 详细 -> 编码)
- `include/Designer/` — public headers
- `src/` — implementation
- `layouts/` — JSON layouts for the designer's own UI
- `build_scripts/` — build scripts (same pattern as UICornerstone)
- `subModules/UICornerstone/` — UICornerstone as git submodule

## Setup & Build

```
git clone --recursive git@github.com:SeaOceanLiu/CornerstoneDesigner.git
build_scripts\\build.bat sdl3
```

- Requires Visual Studio 2022 (C++ desktop workload) + CMake 3.16+
- UICornerstone SDL3 backend (default)
- Build output: `build/sdl3/Debug/CornerstoneDesigner.exe`

## Design Documents (waterfall process)

Read `doc/` in order — each document includes its phase's testing content:

1. `01_需求说明文档.md` — requirements (needs approval before next step)
2. `02_设计规格文档.md` — architecture & spec (needs review)
3. `03_详细设计文档.md` — detailed design (needs review)
4. `04_实施编码文档.md` — implementation plan

## Commit Convention

Match the style of existing commits — currently just `Initial commit`.

## Verification

Before committing, run:

1. `cmake --build build --config Debug` — must compile with zero warnings
2. Run any tests that exist
3. Check submodule: `git submodule status`
