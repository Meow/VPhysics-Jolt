# Building Volt

###### Note: Garry's Mod is built with CMake in Docker instead, see the README.
###### Note: This repository can be built with the default SDK but will not be outlined in this guide.

All build instructions start the same. Clone a local copy of [mini-source-sdk](https://github.com/RaphaelIT7/mini-source-sdk/tree/gmod-changes). (A minified version of [Source SDK](https://github.com/ValveSoftware/source-sdk-2013) with support for Linux and added support for [Visual Studio 2022](https://visualstudio.microsoft.com/vs/) and the [Windows 11 SDK](https://developer.microsoft.com/en-us/windows/downloads/windows-sdk/))
```bash
git clone -b gmod-changes https://github.com/RaphaelIT7/mini-source-sdk.git
```

Following your clone of the mini-source-sdk, navigate to the "version" you would like to build (`sdk2013-mp`, `sdk2013-sp`, `asw`). As an example, the multiplayer version will be used from now on.
```bash
cd mini-source-sdk/sdk2013-mp/src/
```

At this time you should now recursively git clone the VPhysics-Jolt repository into your "src" directory.
```bash
git clone --recursive https://github.com/RaphaelIT7/VPhysics-Jolt.git vphysics_jolt
```

<strong>From this point onward, please follow the rest of the guide specific to your chosen build platform. The current working directory is also assumed to be `mini-source-sdk/sdk2013-mp/src/` and should be adjusted to suit your own build if need be.</strong>

## Windows

Continuing on Windows, be sure you have both [Visual Studio 2022](https://visualstudio.microsoft.com/vs/) and the [Windows 11 SDK](https://developer.microsoft.com/en-us/windows/downloads/windows-sdk/) installed on your system. (even if you run Windows 10 the SDK still applies)

### CLI

The next step is to run the fix registry script (as administrator). You can just Right-Click -> 'Run As Administrator' the `fix_registry.bat` file located in `mini-source-sdk/sdk2013-mp/src/`. If your command prompt or powershell instance are already running under administrator you can run the script from there:
```bash
.\fix_registry.bat
```

This is the final command to run before the CLI portion of this guide ends. It will generate your [Visual Studio](https://visualstudio.microsoft.com/vs/) project/solution file(s). (You may also just Double-Click the file `createjoltprojects.bat`)
```bash
.\createjoltprojects.bat 32
```

Pass `64` instead of `32` for a 64-bit build.

### Visual Studio

After running `createjoltprojects.bat` you should now have a file named `jolt.sln`. Open it. After opening, building should be simple. Press the shortcut `Ctrl+Shift+B` *OR* find the `Solution 'jolt'` bar in the Solution explorer window and Right-Click -> `Build Solution`.

Congratulations!

The built `vphysics.dll` wrapper and the `vphysics_jolt_sse2.dll`, `vphysics_jolt_sse42.dll` and `vphysics_jolt_avx2.dll` modules should now be located in `mini-source-sdk/sdk2013-mp/game/bin/`. Copy all of them.

### Build errors

Common build errors you may run into building on Windows and their solutions.

> #### memoverride.cpp

If there is an error involving a symbol named ` size_t _msize` or `size_t __cdecl _msize_base`, it is most likely because you haven't updated your Windows SDK to the [Windows 11 SDK](https://developer.microsoft.com/en-us/windows/downloads/windows-sdk/). Please do so. If you still get this error make sure that your Visual Studio projects also default to the latest installed version if they don't already.

## Linux

Continuing on Linux, there are a few prerequisites to building regardless of distribution.
- gcc 11+
- g++ 11+
- gcc-multilib
- g++-multilib
- make

### Debian/Ubuntu
###### Note: Certain versions may also require version explicit packages (gcc/g++ 11 may not be the default installed version; Debian 12 and Ubuntu 22.04 have it).

Before we continue building project files, lets get our dependencies out of the way.
Install the `build-essential` group to get common developer packages such as gcc, g++, and make, which are included.
```bash
sudo apt install build-essential
```

After installing the build-essential group packages, we now need to install the x86 dependencies.
```bash
sudo apt install gcc-multilib g++-multilib
```

The next step is to build our makefiles by running the premade script `createjoltprojects.sh`
```bash
./createjoltprojects.sh 32
```

Once the script has finished running you should now have a file name `jolt.mak` in the current directory. Verify you do and now run it with the command below. This command automatically will (and is recommended to) use all available CPU threads. If you want to specify it yourself, replace `$(nproc)` with a number less than or equal to your max number of threads. To be clear, this only affects compile speed.
```bash
make -j $(nproc) -f jolt.mak
```

Alternative example...
```bash
make -j 8 -f jolt.mak
```

Congratulations!

If the console outputs `LINKING` *and* `COPYING TO` messages, that means your build was successful and you may retrieve the `vphysics.so` wrapper and the `vphysics_jolt_sse2.so`, `vphysics_jolt_sse42.so` and `vphysics_jolt_avx2.so` modules, which will be located in `mini-source-sdk/sdk2013-mp/game/bin/`.

### Build errors (distribution independent)

Common build errors you may run into building on Linux and their solutions.

> #### error: unrecognized command line option ‘-std=gnu++20’; did you mean ‘-std=gnu++2a’? (make: *** [jolt.mak:28: all] Error 2)

This error is caused because the compile was not started with gcc/g++ v11 or higher. Please update.

> #### error: '__builtin_bit_cast' was not declared in this scope

Same cause: Jolt needs gcc/g++ v11 or higher.

## OSX
Unsupported.

¯\\\_(ツ)\_/¯
