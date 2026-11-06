# Ruffle in Teletext Navigator: how this tree is used

This directory is the Flash player of the Retro Web Browser tab. It builds
one file, `nav_ruffle.dll`, which `src/web_flash.c` loads at run time from
the folder `TeletextNavigator.exe` is in. Design, tests and gates:
`docs/2026-11-05_RETROWEB_RUFFLE.md`.

## What is here

| path | what | origin |
| --- | --- | --- |
| `core/`, `swf/`, `flv/`, `wstr/`, `render/` (`src`, `wgpu`, `naga-agal`, `naga-pixelbender`, `pixel_bender`), `video/` (`src`, `software`), `tools/asc/`, `LICENSE.md`, `README.md` | Ruffle, **unmodified** | github.com/ruffle-rs/ruffle tag `nightly-2026-11-03`, commit `489fc3a99aabb4a17e0dc1e0afc729f83b03cd8f`, version `0.8.0-nightly.2026.11.3` |
| `Cargo.toml` | Ruffle's workspace manifest, **two changes** (below) | same commit |
| `Cargo.lock` | Ruffle's lock file at that commit, pruned by cargo to this workspace | same commit |
| `nav_ruffle/` | the C ABI shim (`navrfl_*`), written for Navigator | this project |
| `vendor/` | every crate the workspace depends on, written by `cargo vendor --versioned-dirs --locked` | crates.io and the git revisions listed in `.cargo/config.toml` |
| `.cargo/config.toml` | points cargo at `vendor/`, offline | written by `cargo vendor` |
| `build_nav_ruffle.bat`, `README-NAVIGATOR.md` | this project | |

Only the Ruffle crates the DLL uses were copied. Left out: the desktop and
web front ends, the extension, `frontend-utils` (it pulls in reqwest),
`exporter`, `scanner`, `fuzz`, `tests`, `tools` other than `asc`, the
canvas and WebGL renderers, `video/external` and the pixel_bender assembly
tests. Each copied crate is whole, test fixtures included.

### The two changes to `Cargo.toml`

1. `[workspace] members` lists only the crates above plus `nav_ruffle`;
   `default-members = ["nav_ruffle"]`.
2. A new `[profile.navigator]` (inherits `release`): `panic = "unwind"`,
   thin LTO, 4 codegen units, stripped. Upstream's own `release` profile
   sets `panic = "abort"` and is left as it is. Unwinding is REQUIRED: every
   `navrfl_*` export runs under `catch_unwind`, so a movie that makes
   Ruffle panic is torn down and the page shows its fallback. Under
   `abort` the same panic would end the whole Navigator.

## Build

    third_party\ruffle\build_nav_ruffle.bat

Needs MSYS2 UCRT64 Rust: `pacman -S mingw-w64-ucrt-x86_64-rust` (built and
tested with 1.99.1, LLVM 23.1.3; it adds no runtime DLL to ship). The
script runs `cargo build --profile navigator --locked --offline -p
nav_ruffle` with the source path remapped, then copies
`target\navigator\nav_ruffle.dll` to the repository root beside
`TeletextNavigator.exe`. Nothing is downloaded. `--no-copy` skips the copy.

No Java: Ruffle compiles its ActionScript 3 globals with its own Rust
`asc` (`tools/asc`) at build time.

The C build (`build_x64.bat`) does not need Rust at all: the exe does not
link the DLL, it loads it. `set NAV_FLASH=0` before `build_x64.bat` builds
the exe with no Flash code at all (the no-go switch).

## Keep `vendor/` whole

Every vendored crate lists all of its files in `.cargo-checksum.json`, and
cargo refuses to build if any is missing or changed. A few are files that
broad ignore patterns catch (`vendor/psm-*/src/arch/wasm32.o`,
`vendor/console-*/tests/data/*.log`, `vendor/libloading-*/tests/*.dll`),
which is why the repository's `.gitignore` re-includes
`/third_party/ruffle/vendor/**` as its last rule. Check with

    git ls-files --others --ignored --exclude-standard third_party/ruffle

which must print nothing.

## Updating Ruffle

1. Check out a newer nightly tag; copy the same crate directories over
   these; re-apply the two `Cargo.toml` changes.
2. Copy upstream's `Cargo.lock`, then `cargo vendor --versioned-dirs
   --locked vendor` (with `CARGO_NET_GIT_FETCH_WITH_CLI=true`: MSYS2's
   libgit2 rejects GitHub's certificate) and replace `.cargo/config.toml`
   with what it prints, keeping `[net] offline = true`.
3. Rebuild, then the gates in the docs report (the malformed-SWF set, the
   monitor-2 driver, the render-dump comparison).

## Licences

Ruffle: MIT OR Apache-2.0 at your option (`LICENSE.md`). `nav_ruffle`:
the same. The vendored crates carry their own licence files; all are
permissive (MIT, Apache-2.0, BSD, Zlib, ISC, Unlicense, Unicode-3.0)
except symphonia (MP3 decoding), which is MPL-2.0, and self_cell, taken
under its Apache-2.0 option. The nihav VP6 crates are MIT (`COPYING` in
each; relicensed by their author for Ruffle). See THIRD-PARTY-NOTICES.md
at the repository root.
