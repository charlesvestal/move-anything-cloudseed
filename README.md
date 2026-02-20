# Move Everything CloudSeed

Algorithmic reverb audio effect module for Move Everything, based on CloudSeedCore by Ghost Note Audio.

## Prerequisites

- [Move Everything](https://github.com/charlesvestal/move-anything) installed on your Ableton Move

## Installation

### Via Module Store (Recommended)

1. Launch Move Everything on your Move
2. Select **Module Store** from the main menu
3. Navigate to **Audio FX** → **CloudSeed**
4. Select **Install**

### Manual Installation

```bash
./scripts/install.sh
```

## Features

- **Mix**: Dry/wet blend
- **Decay**: Feedback amount controlling reverb tail length (0.05s to 60s)
- **Size**: Room size (scales delay network times, 20-1000ms)
- **Pre-Delay**: Initial delay before reverb onset (0-500ms)
- **Diffusion**: Input diffuser feedback for density
- **Low Cut**: Highpass filter on input (20-1000 Hz)
- **High Cut**: Lowpass filter on input (400-20000 Hz)
- **Mod Amount**: LFO modulation depth for chorus-like movement
- **Mod Rate**: LFO rate (0-5 Hz)
- **Stereo Width**: Stereo decorrelation (cross-seed)

## Algorithm

Modern algorithmic reverb using allpass diffusion and modulated delay networks:

```
Input --> [Pre-delay] --> [Diffuser Network (4x APF)] --> [Delay Network (4x)] --> Output
                                      ^                           |
                                      |______ Hadamard feedback __|
```

### Signal Flow

1. **Pre-delay**: Simple delay line (0-100ms)
2. **Diffuser Network**: 4 cascaded allpass filters for early diffusion
3. **Delay Network**: 4 modulated delay lines with Hadamard feedback matrix
4. **Damping**: One-pole lowpass per delay line for high-frequency absorption

## Building

```bash
./scripts/build.sh      # Build for ARM64 via Docker
./scripts/install.sh    # Deploy to Move
```

## Parameters

| Parameter | Range | Default | Description |
|-----------|-------|---------|-------------|
| mix | 0.0-1.0 | 0.3 | Dry/wet blend |
| decay | 0.0-1.0 | 0.5 | Reverb tail length |
| size | 0.0-1.0 | 0.5 | Room size |
| predelay | 0.0-1.0 | 0.0 | Pre-delay time |
| diffusion | 0.0-1.0 | 0.7 | Input diffuser feedback |
| low_cut | 0.0-1.0 | 0.0 | Highpass filter frequency |
| high_cut | 0.0-1.0 | 1.0 | Lowpass filter frequency |
| mod_amount | 0.0-1.0 | 0.3 | LFO modulation depth |
| mod_rate | 0.0-1.0 | 0.3 | LFO rate |
| cross_seed | 0.0-1.0 | 0.5 | Stereo width/decorrelation |

## Installation

The module installs to `/data/UserData/move-anything/modules/chain/audio_fx/cloudseed/`

## Credits

- **CloudSeed algorithm**: [Ghost Note Audio / ValdemarOrn](https://github.com/ValdemarOrn/CloudSeed) (MIT License)
- **Move Everything port**: Charles Vestal

## License

MIT License - See LICENSE file for full attribution

## AI Assistance Disclaimer

This module is part of Move Everything and was developed with AI assistance, including Claude, Codex, and other AI assistants.

All architecture, implementation, and release decisions are reviewed by human maintainers.  
AI-assisted content may still contain errors, so please validate functionality, security, and license compatibility before production use.
