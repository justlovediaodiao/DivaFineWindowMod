# DIVA Fine Window

An x64 DLL mod loaded by DIVA Mod Loader that adjusts the FINE timing window in *Hatsune Miku: Project DIVA Mega Mix+*.

## Supported Version

Developed and tested with the following `DivaMegaMix.exe` (Version: v1.04):

```text
SHA-256: 6F4DF714C28515C93B3023E7D6DAFA9095DD8835F762D54DB1726FB8BC04504D
```

The DLL uses a byte-pattern scan instead of hard-coded absolute module addresses. If the pattern no longer matches after a game update, the mod will skip hook installation and report the problem in its log instead of modifying an unknown location.

## Building

Requires Visual Studio 2022, the MSVC x64 toolchain, and CMake 3.21 or later:

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release
```

The resulting file is:

```text
build\Release\DivaFineWindow.dll
```

You can also push the project to GitHub. GitHub Actions will produce a ZIP artifact named `DivaFineWindow-x64`.

## Installation

Place the following structure in the game's `mods` directory:

```text
mods/
└─ DIVA Fine Window/
   ├─ DivaFineWindow.dll
   └─ config.toml
```

DIVA Mod Loader must already be installed in the game's root directory.

Alternatively, you can install the mod using DIVA Mod Manager. If you prefer not to install it as a mod, run the Cheat Engine script in the `doc` directory instead; it provides the same function.

## Configuration

Edit `config.toml` in the mod directory:

```toml
fine_window_ms = 100
```

Default timing windows:

- COOL: ±30 ms
- FINE: ±70 ms
- SAFE: ±100 ms
- Outer window: ±130 ms

The allowed range is 30–130 ms. At 100 ms, inputs that would normally fall within the 70–100 ms SAFE range are classified as FINE first. To retain part of the SAFE range, use a value such as 80 or 90.

Restart the game after changing the configuration.

## How It Works

When loaded, the DLL scans for this byte pattern:

```text
F3 0F 10 4A 18 0F 2F 89 84 32 01 00
```

In the supported version, this location corresponds to `DivaMegaMix.exe+26D31E`:

```asm
movss xmm1,[rdx+18]
```

The DLL allocates a nearby code cave and replaces the entry point with a 5-byte relative jump. The code cave uses the judgment-manager pointer held in `RCX` at that point to write the positive and negative FINE boundaries to `RCX+13274` and `RCX+13278`. It then executes the original instruction and jumps back to the game.

The mod does not modify the executable on disk and does not require Cheat Engine or an external hooking library.

## Logging

The DLL creates the following file in its own directory:

```text
DivaFineWindow.log
```

On success, the log records the configured window size and the matched RVA. It also reports pattern-scan and memory-protection failures.
