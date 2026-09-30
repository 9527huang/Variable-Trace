# stb_image_write (single header)

Writes the PNG files behind **File -> Save Plots to \*.png**. The reference build
of this application ships the same library, which is where the choice comes from:
its binary carries one of this header's markers verbatim, so the plot export there
is built on it too.

## Contents

| File | Role |
|---|---|
| `stb_image_write.h` | the whole library, v1.16 |

Only PNG is used; the header's BMP, TGA, JPEG and HDR writers come along for free
and are left compiled out by the preprocessor because nothing calls them.

## Origin

Taken unmodified from the upstream repository:

| Version | Source |
|---|---|
| v1.16 | https://raw.githubusercontent.com/nothings/stb/master/stb_image_write.h |

The header keeps its original byte content. To refresh it, download the same URL
and overwrite the file; the API has been stable across the v1.x series.

## How it is used

`src/Screenshot/PlotExport.cpp` defines `STB_IMAGE_WRITE_IMPLEMENTATION` in a
single translation unit, so the implementation is compiled once. Every other file
sees only the declarations.

## License

Dual licensed, at the user's choice, between the MIT license and the public
domain (Unlicense). Both are stated inside the header. Neither imposes a
condition on this project's GPLv3.
