# Third-party notices

The port's own code and art are under the MIT license (`LICENSE`). It builds on the following.

## libchdr (source, `vendor/libchdr` submodule)

CHD reader used to extract the game files from `hydro.chd` on first run. Statically linked into `hydro.exe`.
<https://github.com/rtissera/libchdr>, BSD 3-Clause, Copyright Romain Tisserand. Bundled dependencies:

- LZMA SDK (`deps/lzma-*`): public domain.
- miniz (`deps/miniz-*`): MIT.
- Zstandard (`deps/zstd-*`): BSD (dual-licensed BSD/GPLv2; used here under BSD), Copyright Meta Platforms, Inc.

The full license texts are in the submodule.

## SDL 2 (source, `vendor/SDL` submodule)

Game controller input. Statically linked into `hydro.exe`. <https://libsdl.org>, zlib license, Copyright (C) 1997-2025
Sam Lantinga. Its bundled HIDAPI (`src/hidapi`) is used under HIDAPI's BSD-style license. The full license texts are in
the submodule.

## dgVoodoo 2 (headers from the `vendor/dgVoodoo2` submodule; DLLs downloaded at build time)

Glide wrapper and configuration API by Dege. <https://github.com/dege-diosg/dgVoodoo2>, <https://dege.fw.hu>.
`glide2x.dll` (from `dgVoodoo2_87_5.zip`) and `dgVoodooAPI.dll` (from `dgVoodooAPI_287_5.zip`) are shipped unmodified
next to `hydro.exe`, as dgVoodoo's author permits for applications. dgVoodoo is freeware and is not covered by this
project's license.

## LANCommander Interposer (source, `vendor/LANCommander.Interposer` submodule)

Reports races to LANCommander. Its embeddable library, with only match statistics, is statically linked into
`hydro_lancommander.dll`. <https://github.com/LANCommander/LANCommander.Interposer>, MIT, Copyright (c) 2024-2026
LANCommander Contributors. Linked into the same DLL, with their full license texts in their submodules or folders:

- yaml-cpp (`vendor/yaml-cpp` submodule): MIT, Copyright (c) 2008-2015 Jesse Beder.
- MinHook (`vendor/minhook` submodule): BSD 2-Clause, Copyright (C) 2009-2017 Tsuda Kageyu (Hacker Disassembler
  Engine: Copyright (c) 2008-2009 Vyacheslav Patkov).
- YY-Thunks (fetched at build time by the Interposer's `build/Get-YYThunks.ps1`): MIT, Copyright (c) 2018 Chuyu-Team.

## Hydro Thunder

Hydro Thunder is © 1999 Midway Games; the trademark and game belong to their respective owners. This project is not
affiliated with or endorsed by them. It includes no file from the game's disk image: the executable, sounds, models
and textures are read at run time from a `hydro.chd` the user supplies.
