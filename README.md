# MSInfo32 Reimplementation

This repository contains a clean, buildable native Windows reimplementation of
the Windows System Information (`msinfo32`) user experience and core data
collection workflows.

It is an independent reimplementation and does not contain Microsoft binaries,
PDBs, decompiler projects, or recovered proprietary source files.

Implemented recovered workflows:

- Win32 tree/list System Information UI.
- WMI-backed System Summary.
- Hardware Resources categories (DMA, I/O, IRQ, memory, allocations).
- Components categories (display, sound, input, network, ports, storage,
  printers, problem devices, USB).
- Software Environment categories (drivers, environment variables, print
  jobs, network connections, processes, services, startup commands, COM/OLE
  registration, Windows Error Reporting).
- Refresh (`F5`).
- Current-category text export.
- XML/NFO-style full-tree save.
- Remote WMI target from `/computer <name>` or `/computer:<name>`.
- Headless `/report <file>` and `/nfo <file>` modes.

Build with:

```powershell
powershell -ExecutionPolicy Bypass -File .\build.ps1
```

The build output is written to `build/` and is intentionally ignored by Git.
