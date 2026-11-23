# Frosty Bird

<img src="installer/assets/frostybird.png" alt="Frosty Bird" width="200" />

-----

Frosty Bird is an unoffical, fan-led game runtime for Matrix Awaken's Xbox Series release on Linux. The game is fully playable from start to end, although there are still MAJOR graphical bugs.

## Install and play

Download **`FrostyBird-linux-x86_64.zip`** from the release assets, extract the
entire folder, and run **`frostybird`**. Select your original Matrix Awakens
package when prompted. The native C++ installer sets up Wine, registers the
packaged movie decoder, and warms shaders and Vulkan pipelines on your GPU.
When setup finishes, the game launches automatically. Later, run `frostybird`
again or choose **Frosty Bird — The Matrix Awakens** in your applications menu.
The player ZIP requires neither Python nor build tools.

> [!IMPORTANT]
>
> > This repository contains **no game files or assets**. **You must obtain your own copy of the Series X/S files of Matrix Awakens to use this project.** DO NOT ASK WHERE TO GET THESE FILES. Any requests for links to game assets will result in an IMMEDIATE AND PERMENANT BAN.

-----

## System Requirements

- x86_64 CPU with at least the AVX2 feature set or greater
  - Recommended: A **Zen 3** or **12-gen Intel** CPU that runs at 4Ghz or higher.
- A Vulkan 1.4 compatible GPU with ray-tracing
  - Recommended: At least a **RX 6700 XT** or **3060 12GB** to play at 1080p 30FPS.
- 16GB of RAM
- 60GB of free storage
- A recent distro of Linux

-----

## Build from source

On x86-64 Linux, install the [build prerequisites](docs/build.md#prerequisites), then run:

```sh
cmake --preset runtime
cmake --build --preset runtime
```

This builds Wine, vkd3d-proton, DXVK's DXGI, Xodus Gaming Runtime, the audio codec,
and Frosty's host and guest tools. It does not need Steam or the game files to compile.
See the [build guide](docs/build.md) for first launch with your own package, existing
checkouts, and [which forks to publish](docs/build.md#publishing-the-forks).
To produce the downloadable ZIP, see [Packaging the installer](docs/installer.md).

-----

## How does it work?

Frosty Bird works by simulating/emulating an Xbox environment with the help of Wine, VKD3D, and Xodus. The Xbox Series-era APIs that Matrix Awakens expects are implemented. The original game code is ran directly on your CPU without any emulation or static recompliation.

For graphics, the original RDNA 2 shaders are translated and recompiled while a modified version of VKD3D is used to convert D3D12x API calls to Vulkan calls.

-----

## FAQ

- Wait, Linux only? How about Windows?

  - I have no plans to rush into a Windows port. It will eventually happen, but I don't use Windows that much so it's not a priority.
