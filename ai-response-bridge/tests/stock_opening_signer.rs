//! Stock opening signature against the node's honest local fixture; no network/GPU.
use nexus_ai_response_bridge::{stock_escrow::EscrowWatcher, verified_inference::{draw_seed, verify_opening, Failure}};
use keryx_verifiable_inference::{testkit::Model, verify::ResponseCtx};
#[test]
fn stock_signs_honest_opening_and_rejects_signature_mutation() {
    let root=std::env::var("NEXUS_ESCROW_TEST_DIR").expect("persistent test directory required");
    assert!(!root.starts_with("/tmp"));
    let directory=tempfile::tempdir_in(root).unwrap();
    let watcher=EscrowWatcher::new(&hex::encode([7u8;32]),
        "keryx:qp8n2k7uklxq4aegau7vawtptkgxsja4kt99lpv6krctwpq8tpc65uyeddvzr",directory.path().join("escrow.json")).unwrap();
    let signer=keryx_inference::escrow_pubkey_of(&[7;32]).unwrap();
    let model=Model::new();let response=model.response(3,9,true);
    let seed=draw_seed(&[3;32],&[9;32]);
    let body=model.open(&response,[9;32],&seed).encode();
    let payload=watcher.sign_opening(&body);
    let context=ResponseCtx { trace_root:response.trace_root,n_prompt:response.n_prompt,n_pos:response.n_pos };
    assert_eq!(verify_opening(&payload,&[9;32],&signer,&model.tier(),&context,&seed),Ok(()));
    let mut mutated=payload;let last=mutated.len()-1;mutated[last]^=1;
    assert_eq!(verify_opening(&mutated,&[9;32],&signer,&model.tier(),&context,&seed),Err(Failure::Signature));
}
