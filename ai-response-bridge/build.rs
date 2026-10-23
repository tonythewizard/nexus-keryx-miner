fn main() -> Result<(), Box<dyn std::error::Error>> {
    let proto = "../vendor/keryx-miner-v057/proto/rpc.proto";
    println!("cargo:rerun-if-changed={proto}");
    tonic_build::configure()
        .build_client(false)
        .build_server(false)
        .compile(&[proto], &["../vendor/keryx-miner-v057/proto"])?;
    Ok(())
}
