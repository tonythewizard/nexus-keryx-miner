//! CPU-only harness for the pinned upstream v5 proof builder/verifier. Not a
//! production v5 backend: the GPU walk and activation gate must be ported too.
//! The synthetic index below deliberately holds no GGUF, network or wallet data.
#![allow(dead_code)]
pub(crate) mod pom {
    pub fn blake(bytes: &[u8]) -> [u8;32] { *blake3::hash(bytes).as_bytes() }
    pub fn mix64(mut x:u64) -> u64 {
        x ^= x>>30; x=x.wrapping_mul(0xbf58476d1ce4e5b9);
        x ^= x>>27; x=x.wrapping_mul(0x94d049bb133111eb); x^(x>>31)
    }
    fn pair(a:&[u8;32],b:&[u8;32])->[u8;32] {
        let mut bytes=[0;64];bytes[..32].copy_from_slice(a);bytes[32..].copy_from_slice(b);blake(&bytes)
    }
    fn parent(level:&[[u8;32]])->Vec<[u8;32]> {
        level.chunks(2).map(|p|pair(&p[0],p.get(1).unwrap_or(&p[0]))).collect()
    }
    pub fn merkle_root(leaves:&[[u8;32]])->[u8;32] {
        let mut level=leaves.to_vec();while level.len()>1 {level=parent(&level);}level[0]
    }
    pub fn verify_merkle(mut hash:[u8;32],mut index:u64,path:&[[u8;32]],root:&[u8;32])->bool {
        for p in path {hash=if index&1==0 {pair(&hash,p)}else{pair(p,&hash)};index>>=1;}
        hash==*root
    }
    pub struct WeightIndex {pub n_chunks:u64,pub r_t:[u8;32],data:Vec<u8>,levels:Vec<Vec<[u8;32]>>}
    pub fn index_from_ram(data:Vec<u8>)->WeightIndex {
        assert!(!data.is_empty() && data.len()%32==0);
        let mut levels=vec![data.chunks_exact(32).map(blake).collect::<Vec<_>>()];
        while levels.last().unwrap().len()>1 {levels.push(parent(levels.last().unwrap()));}
        WeightIndex{n_chunks:(data.len()/32)as u64,r_t:levels.last().unwrap()[0],data,levels}
    }
    impl WeightIndex {
        pub fn read_chunk_bytes(&self,off:u64)->[u8;32] {self.data[off as usize*32..off as usize*32+32].try_into().unwrap()}
        pub fn merkle_path(&self,off:u64)->Vec<[u8;32]> {
            let mut index=off as usize;let mut path=Vec::new();
            for level in &self.levels[..self.levels.len()-1] {path.push(level.get(index^1).copied().unwrap_or(level[index]));index>>=1;}path
        }
    }
}
// v3 arithmetic used by v4/v5; same wrapping integer formulas as the pinned
// upstream module. No v3 proof-builder or legacy long-running walk is invoked.
pub(crate) mod pom_v3 {
    pub fn fold64(root:&[u8;32])->u64 {u64::from_le_bytes(root[..8].try_into().unwrap())}
    pub fn rho8(acc:i32,tweak:u32)->u8 {
        let mut z=(acc as u32)^tweak;z=z.wrapping_mul(0x9e3779b9);z^=z>>16;
        z=z.wrapping_mul(0x85ebca6b);z^=z>>13;(z&255)as u8
    }
    pub fn rho_tweak(step:u32,row:u32,col:u32)->u32 {
        step.wrapping_mul(0x9e3779b9).wrapping_add(row.wrapping_mul(0xc2b2ae35)).wrapping_add(col.wrapping_mul(0x85ebca6b))
    }
    pub fn dot_i8(row:&[u8],col:&[u8])->i32 {row.iter().zip(col).map(|(&a,&b)|(a as i8 as i32)*(b as i8 as i32)).sum()}
    pub fn snippet_fold(snippet:&[u8;32])->u64 {
        snippet.chunks_exact(4).fold(0,|h,b|super::pom::mix64(h^u32::from_le_bytes(b.try_into().unwrap())as u64))
    }
}
#[path="../../vendor/keryx-trace-abi6/src/pom_v4.rs"]
pub(crate) mod pom_v4;
#[path="../../vendor/keryx-trace-abi6/src/pom_v5.rs"]
mod pom_v5;

#[test]
fn v5_pad_geometry_and_distribution_are_not_v4() {
    use pom_v5::*;
    assert_eq!(POM_V5_PAD_TILES_H14,[638976,3213312,4804608,6471680,2450432]);
    for (weight,pad) in [(64,61),(1,10000),(10000,1)] {
        let steps=v5_pad_steps(weight,pad);
        assert!((1..256).contains(&steps));
        assert_eq!((1..=256).filter(|s|v5_is_pad_step(*s,steps)).count()as u64,steps);
    }
    assert_eq!(POM_V5_TILE_ENTRIES,32);
    assert_eq!(POM_V5_P_ROUNDS,1920);
}
#[test]
fn v5_proof_rejects_changed_weight_paths_and_nonempty_pad_witnesses() {
    use pom_v5::*;
    let blob=(0..64*1024).map(|i|(i*31+7)as u8).collect();let index=pom::index_from_ram(blob);
    let pad_seed=v5_pad_seed(0,&[7;32],&index.r_t);
    let (mut proof,fs)=build_proof_v5(0,123,&index,&pad_seed,61).unwrap();
    assert_eq!(verify_proof_v5(123,&proof,&index.r_t,index.n_chunks,&pad_seed,61).unwrap(),fs);
    let pad=proof.tiles.iter().position(Vec::is_empty).unwrap();proof.tiles[pad]=vec![0;1024];
    assert!(verify_proof_v5(123,&proof,&index.r_t,index.n_chunks,&pad_seed,61).is_err());proof.tiles[pad].clear();
    let weight=proof.tiles.iter().position(|t|!t.is_empty()).unwrap();proof.tiles[weight][0]^=1;
    assert!(verify_proof_v5(123,&proof,&index.r_t,index.n_chunks,&pad_seed,61).is_err());
}
