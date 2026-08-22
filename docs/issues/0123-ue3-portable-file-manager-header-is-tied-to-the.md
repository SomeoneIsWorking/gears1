---
id: 123
title: UE3 portable file-manager header is tied to the Windows CRT
status: resolved
symptom: Using FFileManagerAnsi as the Linux file service fails on io.h, direct.h, _find, and wide Windows CRT APIs
tags: native-ue3,core,files,linux,source-lineage
created: 2026-08-22
updated: 2026-08-22
---

## Root cause

`FFileManagerAnsi.h` looks platform-neutral by name, but it is a 1999 Windows CRT implementation. It includes `io.h` and `direct.h` and implements enumeration, directories, permissions, and wide paths through `_find`, `_w*`, and `_chmod` APIs. Porting it with compatibility macros would preserve Windows semantics as the apparent Linux owner.

## Resolution

Added a first-party Linux boundary with two cohesive layers. `HostFileSystem.*` owns validated UTF-8 conversion and host file semantics behind a C++14-compatible interface with behavior tests. `LinuxFileManager.*` adapts that seam to UE3 `FArchive` and `FFileManagerGeneric` contracts and compiles inside the native Core archive.

## Dead end

Do not include or patch `FFileManagerAnsi.h` for Linux. Keep host policy out of the UE3 archive adapter and test the production host seam directly.
