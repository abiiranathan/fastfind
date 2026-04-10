# FastFind

FastFind is an "Everything"-style instant file search tool for Linux, built with GTK4. It provides lightning-fast file indexing and searching, similar to the popular Everything tool on Windows, but designed for Linux desktops.

## Features
- Instant file search with live filtering
- Background file system indexer
- Exclude lists for directories and files
- Human-readable file size formatting
- Modern GTK4 interface with GListStore and GtkColumnView
- Efficient memory usage with optimized data structures

## Project Structure
- `src/` — Source code
  - `main.c` — Application entry point and GTK UI logic
  - `fileitem.c` / `fileitem.h` — File item abstraction and helpers
  - `indexer.c` / `indexer.h` — Background file system indexer and exclude logic
- `data/` — Desktop integration files
- `build/`, `builddir/` — Build output directories (Meson/Ninja)
- `meson.build` — Meson build configuration
- `install.sh` — Installation script. Installs dependencies based on platform and builds the script.

## Building

Requirements:
- GTK4
- GLib
- Meson
- Ninja

To build:

```sh
meson setup builddir
ninja -C builddir
```

To run:

```sh
./builddir/fastfind
```

## License
See [LICENSE](LICENSE) for details.

## Credits
- Inspired by Everything (voidtools)
- Uses GTK4, GLib, and GIO
