use super::*;
fn binding() -> Binding {
    Binding {
        model: [3; 32],
        root: [4; 32],
        chunks: 1000,
    }
}
fn capture() -> Capture {
    let m = p::TierManifest::for_attn(0).unwrap();
    let ctx = p::chain_ctx(0).unwrap().1;
    let request = [2; 32];
    let prefix = p::prefix_tokens(&ctx, &request);
    let prompt = ctx.offset as usize + prefix.len() + 4;
    let mut tokens = vec![1; prompt + 4];
    tokens[ctx.offset as usize..ctx.offset as usize + prefix.len()].copy_from_slice(&prefix);
    let ubatch = (0..tokens.len())
        .map(|i| keryx_verifiable_inference::verify::expected_ubatch(i as u64, prompt as u32))
        .collect();
    let rec = p::TraceRecord::new_chain(
        &m,
        prompt as u32,
        ubatch,
        vec![[1; 32]; tokens.len() * m.n_leaves()],
        &tokens,
    )
    .unwrap();
    let c = m.chain.unwrap();
    let n = 2 * c.seq_heads as usize * (c.n_attn + c.n_recur) as usize
        + c.seq_heads as usize * c.n_attn as usize;
    Capture {
        tier: 0,
        request,
        max_tokens: 64,
        gen_secs: 1.0,
        trace: store::wire::ResponseTrace {
            record: rec,
            tokens,
            seq: p::SeqTrace::new(vec![[5; 32]; n]),
        },
        plaintext: Vec::new(),
    }
}
fn payload(c: &Capture, secret: &Hash) -> Vec<u8> {
    let mut r = keryx_inference::AiResponsePayload::new(c.request, 140116000, [0x12; 34], 1)
        .with_private_body(vec![9])
        .with_trace(trace_commit(c).unwrap());
    let (pk, sig) = crate::sign_digest(secret, &r.signed_bytes()).unwrap();
    r.responder = Some(keryx_inference::AiResponder {
        escrow_pubkey: pk,
        signature: sig,
    });
    r.serialize()
}
#[test]
fn same_key_audits_chain_in_seed_then_response_order() {
    let (_dir, mut s, id) = setup();
    let other = [0xff; 32];
    let source = &s.pending[&id];
    let capture = Capture::try_from_slice(&borsh::to_vec(&source.capture).unwrap()).unwrap();
    let mut state = source.state.clone();
    state.response = other;
    s.pending.insert(other, Pending { state, capture });
    s.note_anchor(ChainBlock {
        hash: [9; 32],
        daa: 140115000,
        accepted: vec![],
    })
    .unwrap();
    s.chain_page(
        [9; 32],
        &[],
        vec![
            ChainBlock {
                hash: [10; 32],
                daa: 140115001,
                accepted: vec![id, other],
            },
            ChainBlock {
                hash: [11; 32],
                daa: 140115101,
                accepted: vec![],
            },
        ],
        true,
    )
    .unwrap();
    s.resolve_deadlines().unwrap();
    let first = s.pending[&id].state.policy.deadline.unwrap();
    let p = &s.pending[&other];
    let r = &p.capture.trace.record;
    let work = p::service_work_daa(p.capture.tier, r.n_prompt, r.n_pos - r.n_prompt);
    assert_eq!(p.state.policy.deadline, Some(first + 3 * work));
}
fn dummy_piece(id: Hash, accepted: bool) -> Piece {
    let op = opening::AttnOpening {
        response_txid: id,
        ..Default::default()
    };
    let body = opening::seeded(&op.encode(), &[11; 32]).unwrap();
    let payload = signing(&[1; 32], &body).unwrap();
    Piece {
        id: audit_txid(&payload),
        payload,
        path: vec![],
        kind: 1,
        draw_block: None,
        accepted: accepted.then_some(([12; 32], 140115110)),
        deadline: 140115200,
        attempts: 100,
        last_ms: 1000,
    }
}
#[test]
fn openings_survive_ack_retry_budget_and_backlog_until_canonical_acceptance() {
    let (_dir, mut s, id) = setup();
    let piece = dummy_piece(id, false);
    let bytes = piece.payload.clone();
    s.pending.get_mut(&id).unwrap().state.pieces = vec![piece];
    s.restore.clear();
    assert!(s.due(140115201, 2000).unwrap().is_empty());
    assert_eq!(s.due(140115201, 61000).unwrap(), vec![bytes]);
    assert_eq!(s.pending[&id].state.pieces[0].attempts, 101);
    s.pending.get_mut(&id).unwrap().state.pieces[0].accepted = Some(([12; 32], 140115110));
    assert!(s.due(140115201, 122000).unwrap().is_empty());
    s.pending.get_mut(&id).unwrap().state.pieces[0].accepted = None;
    assert!(s.due(140151201, 183000).unwrap().is_empty());
}
#[test]
fn only_unfinished_audits_keep_the_network_backlog_grace() {
    for accepted in [false, true] {
        let (_dir, mut s, id) = setup();
        let p = s.pending.get_mut(&id).unwrap();
        p.state.seed = Some(([11; 32], 140115100));
        p.state.policy.deadline = Some(140115200);
        p.state.pieces = vec![dummy_piece(id, accepted)];
        s.prune(140115401).unwrap();
        assert_eq!(s.has_work(), !accepted);
        if !accepted {
            s.prune(140151201).unwrap();
            assert!(!s.has_work());
        }
    }
}
#[test]
fn replay_failures_continue_after_two_with_backoff_and_no_effect() {
    let (_dir, mut s, id) = setup();
    s.note_anchor(ChainBlock {
        hash: [9; 32],
        daa: 140115000,
        accepted: vec![],
    })
    .unwrap();
    s.chain_page(
        [9; 32],
        &[],
        vec![
            ChainBlock {
                hash: [10; 32],
                daa: 140115001,
                accepted: vec![id],
            },
            ChainBlock {
                hash: [11; 32],
                daa: 140115101,
                accepted: vec![],
            },
        ],
        true,
    )
    .unwrap();
    for attempt in 1..=5 {
        s.pending.get_mut(&id).unwrap().state.policy.replay_retry_ms = 0;
        s.advance(140115102, None, |_, _, _, _, _| {
            Err("mock transient replay".into())
        })
        .unwrap();
        assert_eq!(s.pending[&id].state.failures, attempt);
        assert!(s.pending[&id].state.policy.replay_retry_ms > wall_ms());
        assert!(s.pending[&id].state.pieces.is_empty());
    }
}
#[test]
fn later_metadata_updates_do_not_rewrite_capture_bytes() {
    let (_dir, mut s, id) = setup();
    let path = s.dir.join(format!("{}.trace", hex::encode(id)));
    let before = fs::metadata(&path).unwrap().modified().unwrap();
    let bytes = fs::read(&path).unwrap();
    s.response_intent(id, 1000).unwrap();
    s.response_result(id, false).unwrap();
    assert_eq!(fs::metadata(&path).unwrap().modified().unwrap(), before);
    assert_eq!(fs::read(path).unwrap(), bytes);
}
#[test]
fn early_response_retries_identical_bytes_after_request_acceptance() {
    let (_dir, mut s, id) = setup();
    let payload = s.pending[&id].state.response_payload.clone();
    s.pending.get_mut(&id).unwrap().state.accepted = Some(([7; 32], 140115001));
    s.restore.clear();
    s.chain_page(
        [9; 32],
        &[],
        vec![ChainBlock {
            hash: [10; 32],
            daa: 140115002,
            accepted: vec![[2; 32]],
        }],
        true,
    )
    .unwrap();
    let at = s.pending[&id].state.policy.early_after_ms.unwrap();
    assert!(s.response_due(140115003, at - 1).unwrap().is_empty());
    for n in 0..10 {
        assert_eq!(
            s.response_due(140115003, at + 60000 * n).unwrap(),
            vec![payload.clone()]
        );
    }
    assert!(s.response_due(140115003, at + 600000).unwrap().is_empty());
}
#[test]
fn old_metadata_version_preserves_response_and_unknown_versions_block() {
    let (dir, s, id) = setup();
    let state = &s.pending[&id].state;
    let bytes = borsh::to_vec(state).unwrap();
    let old = StateV2::try_from_slice(
        &bytes[..bytes.len() - borsh::to_vec(&state.policy).unwrap().len()],
    )
    .unwrap();
    let mut old = old;
    old.version = 2;
    let body = borsh::to_vec(&old).unwrap();
    let mut encoded = b"NXH15ST1".to_vec();
    encoded.extend_from_slice(blake3::hash(&body).as_bytes());
    encoded.extend_from_slice(&body);
    let path = s.dir.join(format!("{}.audit", hex::encode(id)));
    drop(s);
    fs::write(&path, &encoded).unwrap();
    let s = Service::open(&dir.path().join("h15"), [1; 32], 0, binding()).unwrap();
    assert_eq!(s.pending[&id].state.response_payload, old.response_payload);
    drop(s);
    let mut body = body;
    body[..4].copy_from_slice(&99u32.to_le_bytes());
    let mut encoded = b"NXH15ST1".to_vec();
    encoded.extend_from_slice(blake3::hash(&body).as_bytes());
    encoded.extend_from_slice(&body);
    fs::write(&path, &encoded).unwrap();
    assert!(Service::open(&dir.path().join("h15"), [1; 32], 0, binding()).is_err());
    assert_eq!(fs::read(path).unwrap(), encoded);
}
#[test]
fn response_recovery_reuses_signed_bytes_and_durable_attempts() {
    let (dir, mut s, id) = setup();
    let original = s.pending[&id].state.response_payload.clone();
    assert_eq!(s.response_for_request([2; 32]), Some(id));
    s.restore.clear();
    s.response_intent(id, 1000).unwrap();
    s.response_result(id, false).unwrap();
    assert!(s.response_due(140115001, 1001).unwrap().is_empty());
    assert_eq!(
        s.response_due(140115001, 4000).unwrap(),
        vec![original.clone()]
    );
    s.refund_unsent(&[id]).unwrap();
    assert_eq!(s.pending[&id].state.response_attempts, 1);
    drop(s);
    let mut s = Service::open(&dir.path().join("h15"), [1; 32], 0, binding()).unwrap();
    assert!(
        s.response_due(140115001, 5000).unwrap().is_empty(),
        "restore must precede a retry"
    );
    s.restore.clear();
    assert_eq!(s.response_due(140115001, 5000).unwrap(), vec![original]);
    assert!(
        s.response_due(140121001, 8000).unwrap().is_empty(),
        "expired response must never be sent"
    );
    s.response_result(id, true).unwrap();
    assert!(s.response_due(140115001, 8000).unwrap().is_empty());
    s.response_intent(id, 9000).unwrap();
    s.response_result(id, false).unwrap();
    s.pending.get_mut(&id).unwrap().state.accepted = Some(([11; 32], 140115001));
    assert!(s.response_due(140115002, 12000).unwrap().is_empty());
}
fn node_row(s: &Service, id: Hash, block: Hash) -> AwaitedOpening {
    let key = secp256k1::SecretKey::from_slice(&s.secret).unwrap();
    let signer = secp256k1::Keypair::from_secret_key(&secp256k1::Secp256k1::new(), &key)
        .x_only_public_key()
        .0
        .serialize();
    AwaitedOpening {
        response: id,
        signer,
        seed: p::trace_seed(&block, &id),
        deadline: 140200000,
        pieces: 15,
        path: vec![],
        accepted_daa: 140115001,
    }
}
fn node_setup() -> (tempfile::TempDir, Service, Hash) {
    let (dir, mut s, id) = setup();
    s.restore.clear();
    s.pending.get_mut(&id).unwrap().state.accepted = Some(([10; 32], 140115001));
    s.chain = vec![
        ChainBlock {
            hash: [10; 32],
            daa: 140115001,
            accepted: vec![id],
        },
        ChainBlock {
            hash: [11; 32],
            daa: 140115101,
            accepted: vec![],
        },
    ];
    (dir, s, id)
}
#[test]
fn expired_replay_requires_positive_node_observation_but_legacy_still_runs() {
    for supported in [false, true] {
        let (_dir, mut s, id) = node_setup();
        s.resolve_deadlines().unwrap();
        let deadline = s.pending[&id].state.policy.deadline.unwrap();
        s.node_supported = supported;
        let mut called = false;
        s.advance(deadline, None, |_, _, _, _, _| {
            called = true;
            Err("probe".into())
        })
        .unwrap();
        assert_eq!(called, !supported);
    }
    let (_dir, mut s, id) = node_setup();
    s.resolve_deadlines().unwrap();
    let deadline = s.pending[&id].state.policy.deadline.unwrap();
    s.node_supported = true;
    s.node_awaits.insert(id, true);
    let mut called = false;
    s.advance(deadline, None, |_, _, _, _, _| {
        called = true;
        Err("probe".into())
    })
    .unwrap();
    assert!(
        called,
        "node backlog authority keeps expired replay eligible"
    );
}
#[test]
fn empty_node_reply_releases_only_queried_ids_after_deadline_and_retains_bytes() {
    let (_dir, mut s, id) = node_setup();
    s.resolve_deadlines().unwrap();
    let deadline = s.pending[&id].state.policy.deadline.unwrap();
    let bytes = s.pending[&id].state.response_payload.clone();
    let unqueried = [77; 32];
    s.node_awaits.insert(unqueried, true);
    assert_eq!(s.awaited_ids(deadline), vec![id]);
    s.apply_awaited(AwaitedFrame {
        daa: deadline,
        openings: vec![],
    })
    .unwrap();
    assert_eq!(s.node_awaits.get(&id), Some(&false));
    assert_eq!(s.node_awaits.get(&unqueried), Some(&true));
    s.prune(deadline).unwrap();
    assert!(s.pending.contains_key(&id));
    let mut called = false;
    s.advance(deadline - 1, None, |_, _, _, _, _| {
        called = true;
        Err("probe".into())
    })
    .unwrap();
    assert!(
        called,
        "an omitted duty before its deadline may still replay"
    );
    s.prune(deadline + 1).unwrap();
    assert!(!s.pending.contains_key(&id));
    let saved = read_state(&s.dir.join(format!("{}.audit", hex::encode(id)))).unwrap();
    assert!(saved.retired);
    assert_eq!(saved.response_payload, bytes);
    assert!(s.dir.join(format!("{}.trace", hex::encode(id))).exists());
}
#[test]
fn failed_node_frame_does_not_release_or_consume_poll_scope() {
    let (_dir, mut s, id) = node_setup();
    s.resolve_deadlines().unwrap();
    assert_eq!(s.awaited_ids(140115200), vec![id]);
    s.node_awaits.insert(id, true);
    assert!(s
        .apply_awaited(AwaitedFrame {
            daa: 0,
            openings: vec![]
        })
        .is_err());
    assert_eq!(s.node_awaits.get(&id), Some(&true));
    assert_eq!(s.awaited_polled, vec![id]);
    s.apply_awaited(AwaitedFrame {
        daa: 140115200,
        openings: vec![],
    })
    .unwrap();
    assert_eq!(s.node_awaits.get(&id), Some(&false));
    assert!(s.awaited_polled.is_empty());
}
#[test]
fn restart_and_old_node_clear_session_observations_without_schema_change() {
    let (dir, mut s, id) = node_setup();
    s.resolve_deadlines().unwrap();
    s.node_supported = true;
    s.node_awaits.insert(id, false);
    s.local_estimates().unwrap();
    assert!(!s.node_supported);
    assert!(s.node_awaits.is_empty());
    s.node_supported = true;
    s.node_awaits.insert(id, false);
    drop(s);
    let s = Service::open(&dir.path().join("h15"), [1; 32], 0, binding()).unwrap();
    assert!(!s.node_supported);
    assert!(s.node_awaits.is_empty());
    assert!(s.pending.contains_key(&id));
}
#[test]
fn node_relisted_response_overrides_negative_poll_and_keeps_backlog_replay() {
    let (_dir, mut s, id) = node_setup();
    s.resolve_deadlines().unwrap();
    s.awaited_ids(140115200);
    s.apply_awaited(AwaitedFrame {
        daa: 140115200,
        openings: vec![],
    })
    .unwrap();
    s.awaited_ids(140115201);
    let row = node_row(&s, id, [11; 32]);
    let deadline = row.deadline;
    s.apply_awaited(AwaitedFrame {
        daa: 140115201,
        openings: vec![row],
    })
    .unwrap();
    assert_eq!(s.node_awaits.get(&id), Some(&true));
    let mut called = false;
    s.advance(deadline + 1, None, |_, _, _, _, _| {
        called = true;
        Err("probe".into())
    })
    .unwrap();
    assert!(called);
    assert!(s.pending.contains_key(&id));
}
#[test]
fn omitted_attention_waits_for_child_draw_before_retirement() {
    let (_dir, mut s, id) = node_setup();
    s.resolve_deadlines().unwrap();
    let mut piece = dummy_piece(id, true);
    let op = opening::AttnOpening {
        response_txid: id,
        partials: vec![1],
        ..Default::default()
    };
    piece.payload = signing(&[1; 32], &opening::seeded(&op.encode(), &[11; 32]).unwrap()).unwrap();
    piece.id = audit_txid(&piece.payload);
    let state = &mut s.pending.get_mut(&id).unwrap().state;
    state.policy.deadline = Some(piece.deadline);
    state.pieces = vec![piece];
    s.node_awaits.insert(id, false);
    s.prune(140115201).unwrap();
    assert!(
        s.pending.contains_key(&id),
        "accepted intermediate phase still awaits child draw"
    );
    s.chain.push(ChainBlock {
        hash: [12; 32],
        daa: 140115110,
        accepted: vec![],
    });
    s.chain.push(ChainBlock {
        hash: [13; 32],
        daa: 140115210,
        accepted: vec![],
    });
    s.prune(140115211).unwrap();
    assert!(!s.pending.contains_key(&id));
}
#[test]
fn child_reorg_invalidates_previous_poll_membership() {
    let (_dir, mut s, id) = node_setup();
    s.resolve_deadlines().unwrap();
    s.pending.get_mut(&id).unwrap().state.pieces = vec![dummy_piece(id, true)];
    s.node_awaits.insert(id, false);
    s.chain_page([11; 32], &[[12; 32]], vec![], true).unwrap();
    assert!(!s.node_awaits.contains_key(&id));
    assert!(s.pending[&id].state.pieces[0].accepted.is_none());
}
#[test]
fn idle_retry_never_writes_but_new_attempt_fails_closed_without_durable_storage() {
    let (_dir, mut s, id) = setup();
    s.restore.clear();
    s.pending.get_mut(&id).unwrap().state.pieces = vec![dummy_piece(id, true)];
    persist(&s.dir, &id, &s.pending[&id].state).unwrap();
    let original = fs::read(s.dir.join(format!("{}.audit", hex::encode(id)))).unwrap();
    let backup = s.dir.with_extension("unavailable");
    fs::rename(&s.dir, &backup).unwrap();
    fs::write(&s.dir, b"unavailable storage").unwrap();
    assert!(s.due(140115201, 61000).unwrap().is_empty());
    s.pending.get_mut(&id).unwrap().state.pieces[0].accepted = None;
    assert!(
        s.due(140115201, 61000).is_err(),
        "no bytes may leave before durable attempt intent"
    );
    assert_eq!(
        fs::read(backup.join(format!("{}.audit", hex::encode(id)))).unwrap(),
        original
    );
}
#[test]
fn metadata_serializer_retains_exact_version4_bytes_and_digest() {
    let (_dir, s, id) = setup();
    for version in [2, 3, 4] {
        let mut state = s.pending[&id].state.clone();
        state.version = version;
        state.pieces = vec![dummy_piece(id, false)];
        persist(&s.dir, &id, &state).unwrap();
        let actual = fs::read(s.dir.join(format!("{}.audit", hex::encode(id)))).unwrap();
        state.version = 4;
        let old_payload = borsh::to_vec(&state).unwrap();
        let mut expected = b"NXH15ST1".to_vec();
        expected.extend_from_slice(blake3::hash(&old_payload).as_bytes());
        expected.extend_from_slice(&old_payload);
        assert_eq!(actual, expected);
        assert_eq!(
            read_state(&s.dir.join(format!("{}.audit", hex::encode(id))))
                .unwrap()
                .pieces[0]
                .payload,
            state.pieces[0].payload
        );
    }
}
#[test]
fn node_deadline_and_seed_are_durable_and_override_estimates() {
    let (dir, mut s, id) = node_setup();
    s.resolve_deadlines().unwrap();
    let row = node_row(&s, id, [11; 32]);
    s.apply_awaited(AwaitedFrame {
        daa: 140115200,
        openings: vec![row.clone()],
    })
    .unwrap();
    assert_eq!(s.pending[&id].state.policy.deadline, Some(row.deadline));
    s.resolve_deadlines().unwrap();
    assert_eq!(s.pending[&id].state.policy.deadline, Some(row.deadline));
    drop(s);
    let s = Service::open(&dir.path().join("h15"), [1; 32], 0, binding()).unwrap();
    assert_eq!(s.pending[&id].state.policy.node, Some(row));
    assert_eq!(s.pending[&id].state.seed, Some(([11; 32], 140115101)));
}
#[test]
fn malformed_ledger_is_rejected_and_unknown_seed_does_not_change_duties() {
    let (_dir, mut s, id) = node_setup();
    s.resolve_deadlines().unwrap();
    let file = s.dir.join(format!("{}.audit", hex::encode(id)));
    let before = fs::read(&file).unwrap();
    let row = node_row(&s, id, [11; 32]);
    for n in 0..4 {
        let mut bad = row.clone();
        match n {
            0 => bad.signer = [0; 32],
            1 => bad.path = vec![u64::MAX],
            2 => bad.pieces = 16,
            _ => bad.accepted_daa = 140115201,
        }
        assert!(s
            .apply_awaited(AwaitedFrame {
                daa: 140115200,
                openings: vec![bad]
            })
            .is_err());
        assert_eq!(fs::read(&file).unwrap(), before);
    }
    assert!(s
        .apply_awaited(AwaitedFrame {
            daa: 140115200,
            openings: vec![row.clone(), row]
        })
        .is_err());
    s.apply_awaited(AwaitedFrame {
        daa: 140115200,
        openings: vec![node_row(&s, id, [99; 32])],
    })
    .unwrap();
    assert_eq!(fs::read(&file).unwrap(), before);
}
#[test]
fn node_reseed_keeps_old_signed_bytes_but_never_submits_them() {
    let (_dir, mut s, id) = node_setup();
    s.resolve_deadlines().unwrap();
    s.pending.get_mut(&id).unwrap().state.pieces = vec![dummy_piece(id, false)];
    let signed = s.pending[&id].state.pieces[0].payload.clone();
    let row = node_row(&s, id, [11; 32]);
    s.apply_awaited(AwaitedFrame {
        daa: 140115200,
        openings: vec![row],
    })
    .unwrap();
    assert_eq!(s.due(140115200, 100000).unwrap(), vec![signed.clone()]);
    s.chain.push(ChainBlock {
        hash: [12; 32],
        daa: 140115150,
        accepted: vec![],
    });
    let row = node_row(&s, id, [12; 32]);
    s.apply_awaited(AwaitedFrame {
        daa: 140115200,
        openings: vec![row],
    })
    .unwrap();
    assert!(s.due(140115200, 200000).unwrap().is_empty());
    assert_eq!(s.pending[&id].state.cached[0].payload, signed);
}
#[test]
fn node_attention_path_and_piece_mask_filter_retry_effects() {
    let (_dir, mut s, id) = node_setup();
    s.resolve_deadlines().unwrap();
    s.pending.get_mut(&id).unwrap().state.pieces = vec![dummy_piece(id, false)];
    let mut row = node_row(&s, id, [11; 32]);
    row.pieces = 2;
    row.path = vec![0];
    s.apply_awaited(AwaitedFrame {
        daa: 140115200,
        openings: vec![row.clone()],
    })
    .unwrap();
    assert!(s.due(140115200, 100000).unwrap().is_empty());
    row.path.clear();
    row.pieces = 1;
    s.apply_awaited(AwaitedFrame {
        daa: 140115200,
        openings: vec![row],
    })
    .unwrap();
    assert!(s.due(140115200, 200000).unwrap().is_empty());
}
#[test]
fn span_admission_prioritizes_ready_audit_before_next_inference() {
    let (_dir, mut s, id) = node_setup();
    s.resolve_deadlines().unwrap();
    s.pending.get_mut(&id).unwrap().state.policy.deadline = Some(140215200);
    let mut called = false;
    s.advance(140115200, Some(1), |_, _, _, _, _| {
        called = true;
        Err("probe".into())
    })
    .unwrap();
    assert!(!called);
    s.next_span = 300000;
    s.advance(140115200, Some(1), |_, _, _, _, _| {
        called = true;
        Err("probe".into())
    })
    .unwrap();
    assert!(called);
}
#[test]
fn older_node_clears_stale_authority_without_changing_signed_response() {
    let (_dir, mut s, id) = node_setup();
    s.resolve_deadlines().unwrap();
    let bytes = s.pending[&id].state.response_payload.clone();
    let row = node_row(&s, id, [11; 32]);
    s.apply_awaited(AwaitedFrame {
        daa: 140115200,
        openings: vec![row],
    })
    .unwrap();
    s.local_estimates().unwrap();
    s.resolve_deadlines().unwrap();
    assert!(s.pending[&id].state.policy.node.is_none());
    assert_eq!(s.pending[&id].state.response_payload, bytes);
}
#[test]
fn schema3_metadata_is_read_without_losing_early_response_policy() {
    let (dir, mut s, id) = setup();
    s.pending.get_mut(&id).unwrap().state.policy.early_sends = 5;
    s.pending.get_mut(&id).unwrap().state.policy.early_after_ms = Some(9000);
    let state = &s.pending[&id].state;
    let mut bytes = borsh::to_vec(state).unwrap();
    assert_eq!(bytes.pop(), Some(0));
    bytes[..4].copy_from_slice(&3u32.to_le_bytes());
    let legacy = StateV3::try_from_slice(&bytes).unwrap();
    assert_eq!(legacy.version, 3);
    let file = s.dir.join(format!("{}.audit", hex::encode(id)));
    let expected = state.response_payload.clone();
    drop(s);
    let mut wire = b"NXH15ST1".to_vec();
    wire.extend_from_slice(blake3::hash(&bytes).as_bytes());
    wire.extend(bytes);
    fs::write(&file, wire).unwrap();
    let s = Service::open(&dir.path().join("h15"), [1; 32], 0, binding()).unwrap();
    assert_eq!(s.pending[&id].state.response_payload, expected);
    assert!(s.pending[&id].state.policy.node.is_none());
    assert_eq!(s.pending[&id].state.policy.early_sends, 5);
    assert_eq!(s.pending[&id].state.policy.early_after_ms, Some(9000));
}
fn setup() -> (tempfile::TempDir, Service, Hash) {
    let dir = tempfile::tempdir().unwrap();
    let mut s = Service::open(&dir.path().join("h15"), [1; 32], 0, binding()).unwrap();
    let c = capture();
    let bytes = payload(&c, &[1; 32]);
    let id = s.commit(c, bytes, 140115000, [9; 32], 140121000).unwrap();
    (dir, s, id)
}
#[test]
fn capture_is_durable_before_effects_and_restarts_with_identity_binding() {
    let (dir, s, id) = setup();
    assert!(s.has_work());
    assert_eq!(s.anchors(), vec![[9; 32]]);
    drop(s);
    let s = Service::open(&dir.path().join("h15"), [1; 32], 0, binding()).unwrap();
    assert!(s.pending.contains_key(&id));
    assert!(Service::open(&dir.path().join("h15"), [2; 32], 0, binding()).is_err());
    let mut changed = binding();
    changed.model = [7; 32];
    assert!(Service::open(&dir.path().join("h15"), [1; 32], 0, changed).is_err());
}
#[test]
fn incomplete_pre_effect_commit_is_retained_and_not_treated_as_sent() {
    let (dir, s, id) = setup();
    let path = s.dir.join(format!("{}.audit", hex::encode(id)));
    let trace = s.dir.join(format!("{}.trace", hex::encode(id)));
    let original = fs::read(&trace).unwrap();
    drop(s);
    fs::remove_file(path).unwrap();
    let s = Service::open(&dir.path().join("h15"), [1; 32], 0, binding()).unwrap();
    assert!(!s.has_work());
    assert_eq!(fs::read(trace).unwrap(), original);
}
#[test]
fn corrupt_metadata_is_preserved_and_blocks_recovery() {
    let (dir, s, id) = setup();
    let file = s.dir.join(format!("{}.audit", hex::encode(id)));
    drop(s);
    fs::write(&file, b"broken").unwrap();
    assert!(Service::open(&dir.path().join("h15"), [1; 32], 0, binding()).is_err());
    assert_eq!(fs::read(file).unwrap(), b"broken");
}
#[test]
fn selected_chain_draw_uses_actual_blocks_and_survives_replay_failure() {
    let (_dir, mut s, id) = setup();
    s.note_anchor(ChainBlock {
        hash: [9; 32],
        daa: 140115000,
        accepted: vec![],
    })
    .unwrap();
    s.chain_page(
        [9; 32],
        &[],
        vec![
            ChainBlock {
                hash: [10; 32],
                daa: 140115001,
                accepted: vec![id],
            },
            ChainBlock {
                hash: [11; 32],
                daa: 140115500,
                accepted: vec![],
            },
        ],
        true,
    )
    .unwrap();
    let mut calls = 0;
    s.advance(140115501, None, |_, seed, _, _, _| {
        assert_eq!(seed, [11; 32]);
        calls += 1;
        Err("transient replay".into())
    })
    .unwrap();
    assert_eq!(calls, 1);
    assert_eq!(s.pending[&id].state.seed, Some(([11; 32], 140115500)));
    s.advance(140115502, None, |_, _, _, _, _| {
        panic!("retry backoff ignored")
    })
    .unwrap();
    s.chain_page(
        [10; 32],
        &[[11; 32]],
        vec![ChainBlock {
            hash: [12; 32],
            daa: 140115600,
            accepted: vec![],
        }],
        true,
    )
    .unwrap();
    s.advance(140115601, None, |_, seed, _, _, _| {
        assert_eq!(seed, [12; 32]);
        Err("transient replay".into())
    })
    .unwrap();
}
#[test]
fn permanent_rpc_failure_expires_only_after_the_ledger_horizon_and_keeps_bytes() {
    let (_dir, mut s, id) = setup();
    let path = s.dir.join(format!("{}.trace", hex::encode(id)));
    let before = fs::read(&path).unwrap();
    s.pending.get_mut(&id).unwrap().state.accepted = Some(([10; 32], 140115001));
    s.prune(140187000).unwrap();
    assert!(s.has_work());
    s.prune(140187001).unwrap();
    assert!(!s.has_work());
    assert!(s.chain.is_empty());
    assert!(s.anchors().is_empty());
    assert!(s.restore.is_empty());
    assert_eq!(fs::read(path).unwrap(), before);
}
#[test]
fn signed_response_and_trace_bytes_cannot_be_substituted() {
    let mut c = capture();
    let bytes = payload(&c, &[1; 32]);
    c.trace.tokens[0] = 9;
    assert!(validate_response(&[1; 32], &bytes, &c).is_err());
    let mut c = capture();
    let bytes = payload(&c, &[1; 32]);
    c.request = [4; 32];
    assert!(validate_response(&[1; 32], &bytes, &c).is_err());
}
