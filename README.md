# cl

A tiny Windows wrapper around Microsoft's `cl.exe` that lets you compile C/C++
from any command prompt without running `vcvars64.bat` (or a developer prompt)
first.

## What it does

On every invocation the wrapper:

1. Locates the newest Visual Studio installation with the x64 C++ tools via
   `vswhere.exe`.
2. Runs `vcvars64.bat` in a child `cmd.exe` and imports the resulting
   environment into the current process. The result is cached under
   `%LOCALAPPDATA%\cl-wrapper` and invalidated when the toolchain or environment
   changes, so subsequent runs are fast.
3. Launches the real `cl.exe` with the original command line untouched —
   arguments, quoting, spaces and response files are preserved. Its exit code
   is passed back to the caller.

The first time it runs it also adds the directory containing `cl.exe` to your
**user** `PATH`, so you can simply type `cl` from any directory. On every run it
checks that recorded location; if the executable has been moved it silently
updates the `PATH` entry to point at the directory it is running from now.

## Requirements

- Windows 10 or newer.
- Visual Studio with the **Desktop development with C++** workload (the
  `Microsoft.VisualStudio.Component.VC.Tools.x86.x64` component).

## Installation

Download `cl.exe` from the latest release and place it anywhere you like, for
example `C:\Tools\cl\cl.exe`. Then run it once:

```bat
cl.exe
```

That first run registers its folder in your user `PATH`. Open a new terminal
(or sign out and back in) and `cl` will be available everywhere:

```bat
cl /?
```

If you move the folder later, just run the binary from its new location once
and the `PATH` entry will be updated automatically.

## Building from source

With the Visual Studio x64 C++ tools available:

```bat
cl /nologo /std:c++17 /EHsc /O2 /MD /Fe:cl.exe cl.cpp
```

Or trigger the **Build and Release** workflow manually in GitHub Actions. It
builds `cl.exe` with MSVC and attaches it to a release.

## License

Released into the public domain under the [Unlicense](LICENSE).
