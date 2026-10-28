use blake2b_simd::Params;
use keryx_inference::{escrow_pubkey_of, seal_request, AiRequestPayload};
use secp256k1::{Keypair, Message, Secp256k1, SecretKey};
use std::{env, fs::OpenOptions, io::Write, path::Path};
#[cfg(unix)]
use std::os::unix::fs::OpenOptionsExt;

fn hex32(value: &str) -> Result<[u8; 32], String> {
    if value.len() != 64 { return Err("model ID must be 32-byte hex".into()); }
    let mut out = [0u8; 32];
    for i in 0..32 { out[i] = u8::from_str_radix(&value[i*2..i*2+2], 16).map_err(|_| "invalid model ID")?; }
    Ok(out)
}

fn main() -> Result<(), String> {
    let args: Vec<String> = env::args().collect();
    if args.len() != 5 { return Err("usage: fixture public|private MODEL_ID_HEX PROMPT OUTPUT".into()); }
    let model = hex32(&args[2])?;
    let prompt = args[3].as_bytes();
    let mut requester_secret = Vec::new();
    let escrow_secret = [0x22u8; 32];
    let escrow_pubkey = escrow_pubkey_of(&escrow_secret).map_err(|_| "test escrow key invalid")?;
    let payout_secret = SecretKey::from_slice(&[0x11u8; 32]).map_err(|_| "test payout key invalid")?;
    let payout_keypair = Keypair::from_secret_key(&Secp256k1::new(), &payout_secret);
    let mut cert_message = Params::new().hash_length(32).to_state();
    cert_message.update(b"KeryxEscrowDelegationV1");
    cert_message.update(&escrow_pubkey);
    let cert_digest = cert_message.finalize();
    let cert_msg = Message::from_digest_slice(cert_digest.as_bytes()).map_err(|_| "delegation digest invalid")?;
    let cert = Secp256k1::new().sign_schnorr_no_aux_rand(&cert_msg, &payout_keypair);
    let payload = match args[1].as_str() {
        "public" => AiRequestPayload::new(model, 16, 20_000_000, 30_000_000, prompt.to_vec()).serialize(),
        "private" => {
            let (request, secret) = seal_request(model, 16, 20_000_000, 30_000_000, prompt, &[escrow_pubkey])
                .map_err(|_| "official private fixture creation failed")?;
            requester_secret = hex::decode(secret.to_hex()).map_err(|_| "fixture secret encoding")?;
            request.serialize()
        }
        _ => return Err("fixture kind must be public or private".into()),
    };
    let path = Path::new(&args[4]);
    // This helper creates synthetic fixtures with repeated test identities.
    // Windows inherits the selected test directory's ACL, never a production key.
    let mut options = OpenOptions::new();
    options.write(true).create_new(true);
    #[cfg(unix)]
    options.mode(0o600);
    let mut file = options.open(path)
        .map_err(|e| format!("cannot create isolated fixture: {e}"))?;
    file.write_all(&(payload.len() as u32).to_le_bytes()).map_err(|e| e.to_string())?;
    file.write_all(&payload).map_err(|e| e.to_string())?;
    file.write_all(&[requester_secret.len() as u8]).map_err(|e| e.to_string())?;
    file.write_all(&requester_secret).map_err(|e| e.to_string())?;
    file.write_all(&escrow_pubkey).map_err(|e| e.to_string())?;
    file.write_all(cert.as_ref()).map_err(|e| e.to_string())?;
    file.sync_all().map_err(|e| e.to_string())?;
    println!("fixture=created kind={} payload_bytes={}", args[1], payload.len());
    Ok(())
}
