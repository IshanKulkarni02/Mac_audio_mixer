use std::env;

fn main() {
    let crate_dir = env::var("CARGO_MANIFEST_DIR").unwrap();
    let config = cbindgen::Config::from_file(format!("{crate_dir}/cbindgen.toml"))
        .expect("cbindgen.toml should parse");

    std::fs::create_dir_all(format!("{crate_dir}/include")).unwrap();

    cbindgen::Builder::new()
        .with_crate(&crate_dir)
        .with_config(config)
        .generate()
        .expect("cbindgen should generate the header")
        .write_to_file(format!("{crate_dir}/include/mixer_ffi.h"));

    println!("cargo:rerun-if-changed=src/lib.rs");
}
