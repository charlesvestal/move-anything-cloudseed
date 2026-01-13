# Move Anything CloudSeed

Algorithmic reverb audio effect module for Move Anything, based on CloudSeedCore by Ghost Note Audio.

## Features

- **Decay**: Feedback amount controlling reverb tail length
- **Mix**: Dry/wet blend
- **Pre-Delay**: Initial delay before reverb onset (0-100ms)
- **Size**: Room size (scales delay network times)
- **Damping**: High-frequency absorption (darker at higher values)

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
| decay | 0.0-1.0 | 0.5 | Reverb tail length |
| mix | 0.0-1.0 | 0.3 | Dry/wet blend |
| predelay | 0.0-1.0 | 0.1 | Pre-delay time |
| size | 0.0-1.0 | 0.5 | Room size |
| damping | 0.0-1.0 | 0.5 | High-frequency damping |

## Installation

The module installs to `/data/UserData/move-anything/modules/chain/audio_fx/cloudseed/`

## Credits

- **CloudSeed algorithm**: [Ghost Note Audio / ValdemarOrn](https://github.com/ValdemarOrn/CloudSeed) (MIT License)
- **Move Anything port**: Charles Vestal

## License

MIT License - See LICENSE file for full attribution
