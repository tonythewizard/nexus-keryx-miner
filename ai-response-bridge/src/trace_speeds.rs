//! Process-lifetime generation/replay estimates from official miner 48d31ba7.

    use std::collections::BTreeMap;
    use std::sync::Mutex;

    /// DAA scores per second of the network.
    const DAA_PER_SEC: f64 = 10.0;
    /// Weight of a new measurement in the running averages.
    const EMA: f64 = 0.3;
    /// Fewest generated tokens for a generation to update the speed.
    const MIN_TOKENS: u32 = 32;
    /// Prompt positions per generated token above which a generation does not update the speed.
    const PROMPT_PER_TOKEN: u32 = 16;
    /// Largest step of one measurement over the running speed.
    const OUTLIER: f64 = 4.0;
    /// Replay over generation durations assumed before one is measured.
    const FIRST_RATIO: f64 = 3.0;
    const PHASE_RATIO: f64 = 2.0;

    #[derive(Default, Clone, Copy)]
    struct Speed {
        secs_per_token: Option<f64>,
        first_ratio: Option<f64>,
        phase_ratio: Option<f64>,
    }

    static SPEEDS: Mutex<BTreeMap<[u8; 32], Speed>> = Mutex::new(BTreeMap::new());

    fn ema(old: Option<f64>, new: f64) -> Option<f64> {
        if !new.is_finite() || new <= 0.0 {
            return old;
        }
        Some(old.map_or(new, |o| o + EMA * (new - o)))
    }

    fn get(model: &[u8; 32]) -> Speed {
        SPEEDS.lock().unwrap_or_else(|p| p.into_inner()).get(model).copied().unwrap_or_default()
    }

    /// Records a generation of `n_gen` tokens after a prompt of `n_prompt` that took `secs`;
    /// only generations the prompt weighs little in count, each capped at a few times the
    /// running speed.
    pub fn note_generation(model: &[u8; 32], n_prompt: u32, n_gen: u32, secs: f64) {
        if n_gen < MIN_TOKENS.max(n_prompt / PROMPT_PER_TOKEN) {
            return;
        }
        let mut m = SPEEDS.lock().unwrap_or_else(|p| p.into_inner());
        let s = m.entry(*model).or_default();
        let measured = secs / n_gen as f64;
        s.secs_per_token = ema(s.secs_per_token, s.secs_per_token.map_or(measured, |o| measured.min(OUTLIER * o)));
    }

    /// Records a replay that took `ratio` times its generation.
    pub fn note_replay(model: &[u8; 32], phase: bool, ratio: f64) {
        let ratio = ratio.clamp(0.05, 20.0);
        let mut m = SPEEDS.lock().unwrap_or_else(|p| p.into_inner());
        let s = m.entry(*model).or_default();
        if phase {
            s.phase_ratio = ema(s.phase_ratio, ratio);
        } else {
            s.first_ratio = ema(s.first_ratio, ratio);
        }
    }

    /// Estimated duration of a generation of `n_gen` tokens, in DAA; `None` before a measure.
    pub fn generation_daa(model: &[u8; 32], n_gen: u32) -> Option<u64> {
        get(model).secs_per_token.map(|s| (s * n_gen as f64 * DAA_PER_SEC).round() as u64)
    }

    /// Estimated duration of a replay of a response generated in `gen_secs` (or of `n_gen`
    /// tokens when unknown), in DAA; `None` before a measure.
    pub fn replay_daa(model: &[u8; 32], phase: bool, gen_secs: Option<f64>, n_gen: u32) -> Option<u64> {
        let s = get(model);
        let base = gen_secs.or_else(|| s.secs_per_token.map(|t| t * n_gen.max(1) as f64))?;
        let ratio = if phase { s.phase_ratio.unwrap_or(PHASE_RATIO) } else { s.first_ratio.unwrap_or(FIRST_RATIO) };
        Some((base * ratio * DAA_PER_SEC).round() as u64)
    }

    #[cfg(test)]
    mod tests {
        use super::*;

        #[test]
        fn estimates_follow_the_measured_speeds() {
            let model = [0x7Au8; 32];
            assert_eq!(generation_daa(&model, 100), None);
            assert_eq!(replay_daa(&model, false, None, 100), None);
            assert_eq!(replay_daa(&model, false, Some(10.0), 100), Some(300));
            note_generation(&model, 0, 8, 100.0);
            assert_eq!(generation_daa(&model, 100), None, "a short generation is not a speed");
            note_generation(&model, 32_000, 1_000, 200.0);
            assert_eq!(generation_daa(&model, 100), None, "nor one the prompt weighs in");
            note_generation(&model, 100, 1_000, 20.0);
            assert_eq!(generation_daa(&model, 4_096), Some(819));
            note_replay(&model, false, 1.5);
            assert_eq!(replay_daa(&model, false, Some(10.0), 0), Some(150));
            assert_eq!(replay_daa(&model, false, None, 1_000), Some(300));
            assert_eq!(replay_daa(&model, true, Some(10.0), 0), Some(200));
            note_generation(&model, 0, 1_000, 40.0);
            assert_eq!(generation_daa(&model, 1_000), Some(260));
            note_generation(&model, 0, 1_000, 1_000.0);
            assert!(generation_daa(&model, 1_000).unwrap() < 600, "an outlier moves the speed by a bounded step");
        }
    }
