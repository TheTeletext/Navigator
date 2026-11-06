//! Build script for nav_ruffle (Teletext Navigator).
//!
//! The GNU linker stamps the link time into the PE header and the export
//! table, so two builds of the same source would differ in those bytes.
//! Leaving the stamp out makes nav_ruffle.dll reproducible: the DLL built
//! from the published tree is byte for byte the DLL that ships.

fn main() {
    println!("cargo:rustc-link-arg-cdylib=-Wl,--no-insert-timestamp");
    println!("cargo:rerun-if-changed=build.rs");
}
