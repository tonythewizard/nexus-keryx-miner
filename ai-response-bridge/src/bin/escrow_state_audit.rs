use nexus_ai_response_bridge::stock_escrow::EscrowWatcher;
use std::{env, fs, path::PathBuf};

fn main() -> Result<(), Box<dyn std::error::Error>> {
    let args: Vec<String> = env::args().collect();
    if args.len() != 4 {
        return Err("usage: escrow_state_audit PAYOUT ESCROW_KEY_FILE SNAPSHOT".into());
    }
    let key = fs::read_to_string(&args[2])?;
    let key = key.trim();
    if key.len() != 64 || !key.bytes().all(|b| b.is_ascii_hexdigit()) {
        return Err("existing escrow key format invalid".into());
    }
    let path = PathBuf::from(&args[3]);
    if !path.is_absolute() || path.starts_with("/tmp") { return Err("state path must be persistent".into()); }
    let mut watcher = EscrowWatcher::new(key, &args[1], path)?;
    watcher.set_claims_enabled(false);
    let hashes = watcher.start_state_validation();
    println!("ESCROW_IMPORT_READ_ONLY=PASS live_block_hashes_to_validate={}", hashes.len());
    // No handle_block/submit/flush call: Drop sees no dirty transitions and leaves imported files untouched.
    Ok(())
}
