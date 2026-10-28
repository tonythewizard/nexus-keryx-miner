//! Network/DAA rules pinned independently of the still incomplete runtime/verifier port.
//! Matching an activation gate does not attest support for that era.
pub const MINER_COMMIT: &str = "48d31ba7b774301eb1eae8f950169c2bd8392977";
pub const NODE_COMMIT: &str = "d1ab79b3796d38232fab59bbd126fb5d19fb5776";
pub const MAINNET_H15: u64 = 140_115_000;
pub const TESTNET_H15: u64 = 3_500;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Network { Mainnet, Testnet }
impl Network {
    pub fn from_ffi(value: u32) -> Option<Self> {
        match value { 0 => Some(Self::Mainnet), 1 => Some(Self::Testnet), _ => None }
    }
    pub fn h15(self) -> u64 { match self { Self::Mainnet => MAINNET_H15, Self::Testnet => TESTNET_H15 } }
    pub fn trace_active(self, daa: u64) -> bool { daa >= self.h15() }
    pub fn attention_active(self, daa: u64) -> bool { self.trace_active(daa) }
    // Block-task activation was removed from both networks in the pinned official snapshot.
    pub fn block_task_active(self, _daa: u64) -> bool { false }
}

pub const H15: u32 = 1;
pub const ATTENTION: u32 = 2;
pub const BLOCK_TASK: u32 = 4;
pub const INVALID_NETWORK: u32 = u32::MAX;

/// Pure rules query: H15 groups PoM v5, verifiable inference, prefix, audit, chain and seed
/// binding, whose gates coincide upstream. Attention and block tasks have separate flags.
#[no_mangle]
pub extern "C" fn nexus_protocol_flags(network: u32, daa: u64) -> u32 {
    let Some(net) = Network::from_ffi(network) else { return INVALID_NETWORK; };
    (u32::from(net.trace_active(daa)) * H15)
        | (u32::from(net.attention_active(daa)) * ATTENTION)
        | (u32::from(net.block_task_active(daa)) * BLOCK_TASK)
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn exact_official_network_boundaries_and_disabled_gates() {
        for (network, gate, active) in [(0, MAINNET_H15, H15 | ATTENTION), (1, TESTNET_H15, H15 | ATTENTION)] {
            assert_eq!(nexus_protocol_flags(network, gate - 1), 0);
            assert_eq!(nexus_protocol_flags(network, gate), active);
            assert_eq!(nexus_protocol_flags(network, gate + 1), active);
            assert_eq!(nexus_protocol_flags(network, u64::MAX), active);
            assert_eq!(nexus_protocol_flags(network, 0), 0);
        }
        for daa in [0, TESTNET_H15, MAINNET_H15, u64::MAX] {
            assert_eq!(nexus_protocol_flags(2, daa), INVALID_NETWORK);
            assert_eq!(nexus_protocol_flags(u32::MAX, daa), INVALID_NETWORK);
        }
    }
}
