//! Links `libstreamextractcore` (built from the C++ sources in the repository root) and
//! makes the dynamic loader find it: next to the executable or in the bundle
//! for installed builds, and in the CMake build directory for development builds.

use std::env;
use std::fs;
use std::path::{Path, PathBuf};

const BUILD_HINT: &str =
    "cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DSTREAMEXTRACT_BUILD_LIBRARY=ON \
&& cmake --build build";

/// File name of the shared library on the target platform.
fn library_file_name(target_os: &str) -> &'static str {
    match target_os {
        "windows" => "streamextractcore.dll",
        "macos" => "libstreamextractcore.dylib",
        _ => "libstreamextractcore.so",
    }
}

/// Directory holding the library: `STREAMEXTRACT_LIB_DIR`, or the CMake build directory of the repository
/// (`build/`, and `build/Release` for multi-config generators on Windows).
fn find_library_dir(manifest_dir: &Path, file_name: &str) -> PathBuf {
    let candidates: Vec<PathBuf> = match env::var_os("STREAMEXTRACT_LIB_DIR") {
        Some(dir) if !dir.is_empty() => vec![PathBuf::from(dir)],
        _ => {
            let build = manifest_dir.join("..").join("..").join("build");
            vec![build.clone(), build.join("Release")]
        }
    };
    if let Some(dir) = candidates.iter().find(|dir| dir.join(file_name).is_file()) {
        return dir.canonicalize().unwrap_or_else(|_| dir.clone());
    }
    let searched: Vec<String> = candidates
        .iter()
        .map(|dir| dir.display().to_string())
        .collect();
    panic!(
        "\n\nThe streamextract shared library `{file_name}` was not found (looked in: {}).\n\
         Build it first, from the repository root:\n\n    {BUILD_HINT}\n\n\
         or set STREAMEXTRACT_LIB_DIR to the directory that contains it.\n",
        searched.join(", ")
    );
}

/// The directory of the executables being built: `OUT_DIR` is `<target>/<profile>/build/<pkg>/out`.
fn profile_dir() -> Option<PathBuf> {
    let out_dir = PathBuf::from(env::var_os("OUT_DIR")?);
    out_dir.ancestors().nth(3).map(Path::to_path_buf)
}

fn main() {
    let manifest_dir =
        PathBuf::from(env::var_os("CARGO_MANIFEST_DIR").expect("CARGO_MANIFEST_DIR"));
    let target_os = env::var("CARGO_CFG_TARGET_OS").unwrap_or_default();
    let debug = env::var("PROFILE").is_ok_and(|profile| profile == "debug");

    println!("cargo:rerun-if-env-changed=STREAMEXTRACT_LIB_DIR");
    println!("cargo:rerun-if-changed=build.rs");

    // Checked before `tauri_build`, which also looks at the bundled library and would fail less clearly.
    let file_name = library_file_name(&target_os);
    let lib_dir = find_library_dir(&manifest_dir, file_name);
    let lib_file = lib_dir.join(file_name);
    println!("cargo:rerun-if-changed={}", lib_file.display());

    tauri_build::build();

    println!("cargo:rustc-link-search=native={}", lib_dir.display());
    println!("cargo:rustc-link-lib=dylib=streamextractcore");

    match target_os.as_str() {
        "macos" => {
            // Bundled .app: the library is copied into Contents/Frameworks (`bundle.macOS.frameworks`).
            // `tauri_build` already adds `-rpath @executable_path/../Frameworks` for it (and stages a copy
            // in `target/Frameworks` for development builds); repeating it makes the linker warn.
            if debug {
                println!("cargo:rustc-link-arg=-Wl,-rpath,{}", lib_dir.display());
            }
        }
        "windows" => {
            // The loader looks next to the executable: make `cargo run` and `cargo test` work.
            if let Some(profile_dir) = profile_dir() {
                for dir in [profile_dir.clone(), profile_dir.join("deps")] {
                    if fs::create_dir_all(&dir).is_ok() {
                        if let Err(error) = fs::copy(&lib_file, dir.join(file_name)) {
                            println!(
                                "cargo:warning=could not copy {file_name} to {}: {error}",
                                dir.display()
                            );
                        }
                    }
                }
            }
        }
        _ => {
            // .deb and AppImage: the library lives in <prefix>/lib/streamextract, the executable in <prefix>/bin.
            println!("cargo:rustc-link-arg=-Wl,-rpath,$ORIGIN:$ORIGIN/../lib/streamextract");
            if debug {
                println!("cargo:rustc-link-arg=-Wl,-rpath,{}", lib_dir.display());
            }
        }
    }
}
