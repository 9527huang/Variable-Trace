# libdwarf (prebuilt)

The DWARF reader behind the `libdwarf` ELF parser. The reference build of this
application ships the same library, which is where the choice comes from: the
parser has to read whatever GCC, Arm GCC or Keil emit, and libdwarf is the
battle tested implementation of that format.

## Contents

| File | Role |
|---|---|
| `include/windows/libdwarf.h`, `include/windows/dwarf.h` | the two public headers |
| `lib/windows/libdwarf.dll.a` | import library, the only thing the linker needs |
| `lib/windows/libdwarf-2.dll` | the library itself, shipped next to the executable |
| `lib/windows/zlib1.dll`, `lib/windows/libzstd.dll` | runtime dependencies of the above |

`libdwarf-2.dll` imports `zlib1.dll` and `libzstd.dll` by name, so all three have
to sit next to `Variable-Trace.exe`. The build copies them there.

## Origin

Taken unmodified from the MSYS2 mingw-w64 packages:

| Package | Version | Source |
|---|---|---|
| `mingw-w64-x86_64-libdwarf` | 2.3.2-1 | https://mirror.msys2.org/mingw/mingw64/ |
| `mingw-w64-x86_64-zlib` | 1.3.2-2 | https://mirror.msys2.org/mingw/mingw64/ |
| `mingw-w64-x86_64-zstd` | 1.5.7-2 | https://mirror.msys2.org/mingw/mingw64/ |

Each package is a zstd compressed tar archive. Only the files listed above were
kept; the packages also carry `dwarfdump.exe`, `dwarfgen.exe`, `libdwarfp-2.dll`,
pkg-config files and the HTML API documentation, none of which this project uses.
The headers keep their original byte content.

The import library was built against the same mingw-w64 toolchain this project
builds with, so it links without any conversion step.

## Refreshing

```
curl -O https://mirror.msys2.org/mingw/mingw64/mingw-w64-x86_64-libdwarf-<ver>-any.pkg.tar.zst
tar --zstd -xf mingw-w64-x86_64-libdwarf-<ver>-any.pkg.tar.zst \
    mingw64/include/libdwarf-2/libdwarf.h mingw64/include/libdwarf-2/dwarf.h \
    mingw64/lib/libdwarf.dll.a mingw64/bin/libdwarf-2.dll
```

Then check the imports of the new DLL — `objdump -p libdwarf-2.dll | grep "DLL Name"`
— and refresh the zlib and zstd packages the same way if the list changed.

## License

libdwarf is distributed under `LGPL-2.1-only AND BSD-3-Clause AND GPL-2.0-only AND
BSD-2-Clause`, zlib under `Zlib`, zstd under `BSD-3-Clause OR GPL-2.0-only`.

libdwarf is used as a shared library, which is what the LGPL asks for: the user
can replace `libdwarf-2.dll` with a modified build. This project itself is GPLv3,
which the LGPL permits.
