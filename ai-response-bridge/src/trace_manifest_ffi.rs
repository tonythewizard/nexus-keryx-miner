//! Read-only canonical H15 manifest/layout query. No worker admission or network effects.
use crate::{protocol_rules::Network, trace_producer::{TierManifest, TraceNetwork}};

#[repr(C)]
#[derive(Default, Clone, Copy, Debug, PartialEq, Eq)]
pub struct TraceManifestInfo {
    pub application_abi:u32,
    pub n_products:u32,
    pub position_leaves:u32,
    pub n_attn:u32,
    pub n_recur:u32,
    pub heads:u32,
    pub interval:u32,
    pub manifest_root:[u8;32],
}

/// Lengths are bytes for the manifest and rows (five u32 per row) for the layout.
/// Query/short-buffer returns 2 and sets required lengths/info without writing either buffer.
/// Invalid network/tier/era returns a negative code and leaves all outputs unchanged.
/// # Safety
/// Pointer validity, alignment, span lengths and non-aliasing must satisfy
/// the contract in `docs/native-ffi.md`; bounds checks cannot validate allocations.
/// Length/count/info outputs must be writable and aligned. The layer capacity counts rows of FIVE
/// u32 values; its allocation must cover `5 * layer_capacity` u32 elements. A size query may
/// supply null data buffers with zero capacities.
#[no_mangle]
pub unsafe extern "C" fn nexus_ai_h15_manifest(network:u32,tier:u32,accepted_daa:u64,
    manifest:*mut u8,manifest_capacity:usize,manifest_len:*mut usize,
    layers:*mut u32,layer_capacity:usize,layer_count:*mut usize,info:*mut TraceManifestInfo)->i32 {
    if manifest_len.is_null() || layer_count.is_null() || info.is_null() {return -1;}
    std::panic::catch_unwind(|| {
        let Some(net)=Network::from_ffi(network) else {return -1;};
        if tier>4 {return -2;}
        if !net.trace_active(accepted_daa) {return -10;}
        let attn=net.attention_active(accepted_daa);
        let Some(m)=TierManifest::for_tier_on(if network==0 {TraceNetwork::Mainnet} else {TraceNetwork::Testnet},tier as u8,accepted_daa) else {return -2;};
        let Some(raw)=(if attn {TierManifest::raw_attn(tier as u8)} else {TierManifest::raw_chain(tier as u8)}) else {return -2;};
        let Some(chain)=m.chain else {return -2;};
        let rows=m.seq_layers();
        let values=TraceManifestInfo{application_abi:if attn {9} else {8},n_products:m.n_products,
            position_leaves:m.n_leaves() as u32,n_attn:chain.n_attn,n_recur:chain.n_recur,
            heads:chain.seq_heads,interval:if attn {m.seq_interval()} else {0},manifest_root:m.m_t};
        unsafe {*manifest_len=raw.len();*layer_count=rows.len();*info=values;}
        if manifest_capacity<raw.len() || manifest.is_null() || layer_capacity<rows.len() || (!rows.is_empty() && layers.is_null()) {return 2;}
        unsafe {
            std::ptr::copy_nonoverlapping(raw.as_ptr(),manifest,raw.len());
            for (i,row) in rows.iter().enumerate() {std::ptr::copy_nonoverlapping(row.as_ptr(),layers.add(5*i),5);}
        }
        0
    }).unwrap_or(-9)
}

/// Exact replay-scaled next-phase window from the pinned official miner.
#[no_mangle]
pub extern "C" fn nexus_ai_attention_phase_window(tier:u8,max_tokens:u32,n_prompt:u32,n_pos:u32)->u64 {
    crate::trace_producer::attn_phase_window_daa(tier,max_tokens,n_prompt,n_pos)
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn native_layout_query_respects_network_and_buffer_canaries() {
        assert_eq!(std::mem::size_of::<TraceManifestInfo>(),60);
        for tier in 0..5 {
            for (network,daa,abi) in [(0,crate::protocol_rules::MAINNET_H15,9),(1,crate::protocol_rules::TESTNET_H15,9)] {
                let mut info=TraceManifestInfo::default();let(mut bytes,mut rows)=(0,0);
                let query=unsafe {nexus_ai_h15_manifest(network,tier,daa,std::ptr::null_mut(),0,&mut bytes,std::ptr::null_mut(),0,&mut rows,&mut info)};
                assert_eq!(query,2);assert_eq!(info.application_abi,abi);assert!(bytes>16);
                assert_eq!(rows as u32,info.n_attn+info.n_recur);
                let mut raw=vec![0xaa;bytes+8];let mut layout=vec![0xabababab;5*rows+8];
                assert_eq!(unsafe{nexus_ai_h15_manifest(network,tier,daa,raw.as_mut_ptr(),bytes-1,&mut bytes,layout.as_mut_ptr(),rows,&mut rows,&mut info)},2);
                assert!(raw.iter().all(|&v|v==0xaa));assert!(layout.iter().all(|&v|v==0xabababab));
                assert_eq!(unsafe{nexus_ai_h15_manifest(network,tier,daa,raw.as_mut_ptr(),bytes,&mut bytes,layout.as_mut_ptr(),rows,&mut rows,&mut info)},0);
                assert_eq!(&raw[bytes..],&[0xaa;8]);assert_eq!(&layout[rows*5..],&[0xabababab;8]);
                let m=TierManifest::parse(&raw[..bytes]).unwrap();assert_eq!(m.m_t,info.manifest_root);
            }
        }
        for(network,tier,daa,code) in [(2,0,3500,-1),(0,5,u64::MAX,-2),(0,0,3500,-10),(1,0,3499,-10)] {
            let mut info=TraceManifestInfo::default();let(mut n,mut r)=(17,19);
            assert_eq!(unsafe{nexus_ai_h15_manifest(network,tier,daa,std::ptr::null_mut(),0,&mut n,std::ptr::null_mut(),0,&mut r,&mut info)},code);
            assert_eq!((n,r),(17,19));assert_eq!(info,TraceManifestInfo::default());
        }
    }
}
