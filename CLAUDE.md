# CLAUDE.md

Instructions for Claude Code when working with this repository.

## Project Overview

CloudSeed is an audio effect module for Move Anything that provides modern algorithmic reverb. This is an **exact port** of CloudSeedCore by Ghost Note Audio (MIT Licensed).

## Architecture

```
src/
  dsp/
    cloudseed.c         # Main DSP implementation (exact CloudSeedCore port)
    audio_fx_api_v1.h   # Audio FX API (from move-anything)
    plugin_api_v1.h     # Plugin API types (from move-anything)
  module.json           # Module metadata
```

## Key Implementation Details

### Components (Exact from CloudSeedCore)

1. **LcgRandom**: Linear congruential generator (a=22695477, c=1, mod 2^32)
2. **RandomBuffer**: Seed-based random generation with crossSeed for stereo decorrelation
3. **Lp1/Hp1**: One-pole lowpass/highpass filters
4. **Biquad**: Low shelf and high shelf filters for EQ
5. **ModulatedAllpass**: Allpass filter with sine LFO modulation and interpolation
6. **AllpassDiffuser**: 12 cascaded modulated allpass filters
7. **ModulatedDelay**: Delay line with sine modulation
8. **MultitapDelay**: Multi-tap delay with 256 taps
9. **DelayLine**: Combined delay + diffuser + shelf EQ + lowpass damping
10. **ReverbChannel**: Full reverb channel with pre-delay, multitap, diffuser, 12 delay lines

### Signal Flow

```
Input → [HP/LP Filters] → [Pre-delay] → [Multitap] → [Input Diffuser]
    → [8 Delay Lines with feedback] → Output
```

### Buffer Sizes (from reference)

| Buffer | Size | Purpose |
|--------|------|---------|
| Delay lines | 384000 | ~8.7s at 44.1kHz |
| Allpass | 19200 | ~435ms at 44.1kHz |
| Process block | 128 | Block processing size |

### Parameters

| Parameter | Range | Description |
|-----------|-------|-------------|
| mix | 0-1 | Dry/wet blend |
| decay | 0-1 | T60 time (0.05s to 60s via Resp3dec) |
| size | 0-1 | Room size (20-1000ms via Resp2dec) |
| predelay | 0-1 | Pre-delay (0-500ms via Resp2dec) |
| diffusion | 0-1 | Input diffuser feedback |
| low_cut | 0-1 | Highpass filter (20-1000 Hz via Resp4oct) |
| high_cut | 0-1 | Lowpass filter (400-20000 Hz via Resp4oct) |
| mod_amount | 0-1 | LFO modulation depth |
| mod_rate | 0-1 | LFO rate (0-5 Hz via Resp2dec) |
| cross_seed | 0-1 | Stereo decorrelation |

### CrossSeed Stereo Implementation

```
Left channel:  cross_seed = 1.0 - 0.5 * seed_param
Right channel: cross_seed = 0.5 * seed_param
```

## Build Commands

```bash
./scripts/build.sh      # Build for ARM64 via Docker
./scripts/install.sh    # Deploy to Move
```

## Credits

Algorithm based on CloudSeedCore by Ghost Note Audio (MIT Licensed).
https://github.com/GhostNoteAudio/CloudSeedCore
