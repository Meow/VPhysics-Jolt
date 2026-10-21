![VPhysics Jolt Logo](assets/cube_base_nobg.png "VPhysics Jolt")

## What is Volt? ⚡

Volt (VPhysics Jolt) is a replacement for Source's VPhysics which uses IVP/Havok using [Jolt Physics](https://github.com/jrouwe/JoltPhysics/)<br>
Created by [Joshua Ashton (🐸✨)](https://github.com/Joshua-Ashton) [@phys_ballsocket](https://twitter.com/phys_ballsocket) and [Josh Dowell (Slartibarty)](https://github.com/Slartibarty) [@Slartbarty](https://twitter.com/Slartbarty).

Volt is designed to be incredibly high performance, supporting thousands of objects at once without bringing tick/framerate down to a crawl.

In our testing, the performance overhead from having thousands of objects moving at once, now comes from the client code needing to update/render, as going out of the PVS of these objects will still cause them to be simulated, but will no longer be rendered.

## Features

Volt is mostly feature complete, but is missing some things such as full support for raycast vehicles, and perhaps other things we've missed!

It is our goal to implement any missing features at some point.

Below is a feature table of Volt vs VPhysics and the Bullet VPhysics project.
If we missed anything we don't support or we do, feel free to add to it.
It is not meant to be a bash on anyone elses work however, the Bullet VPhysics project was a great inspiration to us!

| Feature       | VPhysics | Volt (VPhysics Jolt) | Bullet VPhysics |
|:--------------|:--------:|:-------------:|:---------------:|
| Constraints (except Pulleys)                                  | ✔️ | ✔️ | ✔️ |
| Pulleys                                                       | ✔️ | ✔️ | ❌ |
| Breakable constraints                                         | ✔️ | ✔️ | ❌ |
| Motors (Motion Controllers)                                   | ✔️ | ✔️ | ✔️ |
| Motors (Constraint)                                           | ✔️ | ✔️ | ❌ |
| Ragdolls                                                      | ✔️ | ✔️ | ✔️ |
| Triggers                                                      | ✔️ | ✔️ | ❌ |
| Object touch callbacks                                        | ✔️ | ✔️ | ❌ |
| Prop damage/breaking                                          | ✔️ | ✔️ | ❌ |
| Fluid events                                                  | ✔️ | ✔️ | ❌ |
| Prop splashing effects                                        | ✔️ | ✔️ | ❌ |
| Wheeled Vehicles                                              | ✔️ | ✔️ | ✔️ |
| Raycast Vehicles (ie. Airboat)                                | ✔️ | 〰️ (Airboat implemented) | 〰️ (janky) |
| NPCs/Doors (Shadow Controllers)                               | ✔️ | ✔️ | ✔️ |
| Save/Restore Support                                          | ✔️ | ✔️ | ❌ |
| Portal Support                                                | ✔️ | ✔️ | ❌ |
| Game per-object collide callback support<br>eg. no-collide    | ✔️ | ✔️ | ❌ |
| Crash-resistant solver                                        | ❌ | ✔️ | (no data) |
| Supports thousands of objects without lag                     | ❌ | ✔️ | ❌ |
| Multithreaded                                                 | ❌ | ✔️ | ✔️ (partially) |
| Proper player controller                                      | ✔️ | ✔️ | ✔️ (partially) |

## Garry's Mod fork

This fork targets Garry's Mod (32-bit and x86-64 branches, Linux and Windows, clients and dedicated servers). Beyond the Docker builds, it reworks how physgun-held objects interact with what they touch and carry (held objects can't press props into the ground, containers push their contents instead of sweeping through them, friction effects use the velocities bodies actually moved with), defers object deletion while the simulation runs, fixes the 64-bit module loading that crashed non-stock server launchers, and makes `vjolt_*` convars settable on 32-bit GMod (see `sdk-patches/`). Any `vjolt_*` convar can also be set at startup through the environment as `VJOLT_CVAR_<name>=<value>`.

### Credit: Avrena's fork

A good part of the GMod fixes here come from [Avrena/VPhysics-Jolt](https://github.com/Avrena/VPhysics-Jolt), who found and fixed them first. Taken from there, as cherry-picks or ports:

- Collision pair hash cleanup across object buckets (`272fd47`).
- Bounding-box collide cache keyed by extents, fixing the `CUtlRBTree` overflow (`105c7fd`).
- Tolerant parsing of dirty `.phy`/vehicle script buffers: BOMs, unterminated quotes, doubled block keywords (`4326612`, `20352bc`), and per-key unknown-key handling in the schema parser.
- Buoyancy from disabled fluid objects (`2a6dcf9`).
- Vehicle axle ratio applied to the Jolt differentials, and non-automatic speed coupling (`d7482e7`, `4f7508e`).
- `SetAngularMotor` dispatching on the actual constraint type, fixing memory corruption on `$animatedfriction` ragdolls (`cb199ad`).
- Rotation-only ragdoll constraints honouring `onlyAngularLimits` (`b4aa0b2`, `a5e1ee5`).
- Constraint group solver iterations (`f994301`, `7724ac4`).
- `SetMass` scaling the existing inertia (`79a4194`), the fixed-constraint enable state (`b01d771`), `vec3_angle` and the velocity reset on teleport (`a5ae69c`), the temp allocator falling back to malloc (`803f3b5`), the `GetObjectList` cache (`17f38f7`, `fef7e5c`), and PreCollision/PostCollision events delivered as adjacent pairs (`7865b55`).

Their Baumgarte and contact-recovery changes, which need their patched Jolt, were not taken.

## Bugs

VPhysics Jolt is not without its flaws, however. See the [issue tracker](https://github.com/RaphaelIT7/VPhysics-Jolt/issues) for bugs that are known.

There are definitely going to be bugs that we don't know about or haven't encountered, or different quirks across engine branches.

## How to build

### Garry's Mod

Garry's Mod builds use CMake in Docker, against the GMod headers in [RaphaelIT7/mini-source-sdk](https://github.com/RaphaelIT7/mini-source-sdk/tree/gmod-changes) (fetched automatically, at a pinned commit):

```bash
git clone --recursive https://github.com/RaphaelIT7/VPhysics-Jolt.git
cd VPhysics-Jolt
./build.sh                 # or: ./build.sh linux32 linux64 win32 win64
```

Patches in `sdk-patches/` are applied to the fetched SDK. Linux builds use the Steam Runtime 3 (sniper) SDK image. Windows builds use MSVC (Visual Studio 2022) under Wine, via [msvc-wine](https://github.com/mstorsjo/msvc-wine); building that image downloads MSVC and accepts the [Visual Studio license](https://go.microsoft.com/fwlink/?LinkId=2086102).

The results go to `dist/`, laid out like the game directory:

| Package | Files | For |
|:--|:--|:--|
| `gmod-linux32` | `bin/vphysics.so` | 32-bit Linux client and listen server (public, prerelease, dev branches) |
| `gmod-linux32-srv` | `bin/vphysics_srv.so` | 32-bit Linux dedicated server (public, prerelease, dev branches) |
| `gmod-linux64` | `bin/linux64/vphysics.so` | 64-bit Linux dedicated server (x86-64 branch) |
| `gmod-linux64-client` | `bin/linux64/vphysics_client.so` | 64-bit Linux client (x86-64 branch) |
| `gmod-win32` | `bin/vphysics.dll` | 32-bit Windows client and dedicated server |
| `gmod-win64` | `bin/win64/vphysics.dll` | 64-bit Windows client and dedicated server (x86-64 branch) |

Each package also has `vphysics_jolt_{sse2,sse42,avx2}` next to the wrapper; the wrapper picks one for the CPU. Copy all of them into the game directory. The `_srv` and `_client` packages differ from the others only in which tier0/vstdlib libraries they link, because those Linux installs name them `libtier0_srv.so` / `libtier0_client.so`.

### Source SDK 2013 and Alien Swarm

These still use VPC. Volt should build fine against Source SDK 2013 and Alien Swarm SDK on MSVC, or GCC 11 or newer, with C++20 support.

*If you are building directly against the public SDK 2013 and Alien Swarm SDKs, you will need to do minor work to memoverride.cpp to make it compatible with the newer compilers and newer Windows SDKs.*

For full build instructions pertaining to SDK 2013, reference [build.md](/build.md).

## Download

Development builds for each commit are available as artifacts on GitHub Actions, for Garry's Mod and for SDK2013 SP/MP + ASW.

Binary builds for each release are on the [Releases](https://github.com/RaphaelIT7/VPhysics-Jolt/releases/) page.

## Media

### Lots of Melons + Dumpster
[![Lots of Melons + Dumpster](https://img.youtube.com/vi/gPDQkmfQCsc/0.jpg)](https://www.youtube.com/watch?v=gPDQkmfQCsc "Lots of Melons + Dumpster")

### Physically Simulated Chain
[![Physically Simulated Chain](https://img.youtube.com/vi/tVmQTmbSJM0/0.jpg)](https://www.youtube.com/watch?v=tVmQTmbSJM0 "Physically Simulated Chain")

### Lots of Balls Test
[![Lots of Balls Test](https://img.youtube.com/vi/tYfiTyRtmz8/0.jpg)](https://www.youtube.com/watch?v=tYfiTyRtmz8 "Lots of Balls Test")

### Wheels + Weld Car Dupe Test
[![Weld Car Dupe Test](https://img.youtube.com/vi/5_QbbXbIrg8/0.jpg)](https://www.youtube.com/watch?v=5_QbbXbIrg8 "Weld Car Dupe Test")

### Door + NPC (Physics Shadowed Objects) Test
[![Door + NPC (Physics Shadowed Objects) Test](https://img.youtube.com/vi/SdEj7HTuJmU/0.jpg)](https://www.youtube.com/watch?v=SdEj7HTuJmU "Door + NPC (Physics Shadowed Objects) Test")

### Lots of Cubes + Ragdolls + Funnel
[![Lots of Cubes + Ragdolls + Funnel](https://img.youtube.com/vi/CLVnSwg33Dk/0.jpg)](https://www.youtube.com/watch?v=CLVnSwg33Dk "Lots of Cubes + Ragdolls + Funnel")

### Slow Mo Cubes
[![Slow Mo Cubes](https://img.youtube.com/vi/GzW_4bufwEk/0.jpg)](https://www.youtube.com/watch?v=GzW_4bufwEk "Slow Mo Cubes")

### Propane in Dumpster
[![Propane in Dumpster](https://img.youtube.com/vi/10vvRJVHGQc/0.jpg)](https://www.youtube.com/watch?v=10vvRJVHGQc "Propane in Dumpster")

*Have some cool media of stuff going on in Volt you'd like to add? Feel free to make a pull request!*

## Projects using Volt

### [Portal 2: Desolation](https://emberspark.games/desolation/)

### [Prelude: Online](https://prelude.online/)

# Have fun! 🐸⚡
