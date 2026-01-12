# CLAUDE.md

Instructions for Claude Code when working with this repository.

## Project Overview

CloudSeed is an audio effect module for Move Anything that provides modern algorithmic reverb based on CloudSeedCore by Ghost Note Audio. It features allpass diffusion and modulated delay networks for rich, diffuse reverb tails.

## Architecture

```
src/
  dsp/
    cloudseed.c         # Main DSP implementation
    audio_fx_api_v1.h   # Audio FX API (from move-anything)
    plugin_api_v1.h     # Plugin API types (from move-anything)
  module.json           # Module metadata
```

## Key Implementation Details

### Audio FX API

Implements Move Anything audio_fx_api_v1:
- `on_load`: Initialize buffers and DSP state
- `on_unload`: Cleanup
- `process_block`: In-place stereo audio processing
- `set_param`: decay, mix, predelay, size, damping
- `get_param`: Returns current parameter values

### DSP Components

1. **Pre-delay**: Simple delay line (0-100ms, 4410 samples max)
2. **Diffuser Network**: 4 cascaded allpass filters with prime-ish delay times
3. **Delay Network**: 4 modulated delay lines with LFO modulation
4. **Hadamard Feedback Matrix**: Mixes delay outputs before feedback
5. **Damping**: One-pole lowpass per delay line

### Signal Flow

```
Input --> [Pre-delay] --> [Diffuser (4x APF)] --> [Delay Network (4x)] --> Output
                                  ^                        |
                                  |___ Hadamard feedback __|
```

### Buffer Sizes

| Buffer | Size | Purpose |
|--------|------|---------|
| Pre-delay | 8192 | Up to ~185ms at 44.1kHz |
| Diffuser | 512 each | 4 allpass buffers |
| Delay | 8192 each | 4 delay line buffers |

### Delay Times (samples at 44.1kHz)

**Diffusers (prime-ish):** 142, 107, 379, 277

**Delay Network (primes):** 2473, 3119, 3947, 4643 (scaled by size parameter)

### LFO Modulation

- Frequency: ~0.3Hz (slow)
- Depth: ~3ms (~132 samples)
- Stereo: L/R have offset LFO phases for width

### Signal Chain Integration

Module declares `"chainable": true` and `"component_type": "audio_fx"` in module.json.

Installs to: `/data/UserData/move-anything/modules/chain/audio_fx/cloudseed/`

## Build Commands

```bash
./scripts/build.sh      # Build for ARM64 via Docker
./scripts/install.sh    # Deploy to Move
```

## Credits

Algorithm based on CloudSeedCore by Ghost Note Audio (MIT Licensed).
