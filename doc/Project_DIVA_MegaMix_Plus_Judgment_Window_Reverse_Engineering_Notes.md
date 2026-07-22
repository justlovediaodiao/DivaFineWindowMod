# Project DIVA Mega Mix+ Judgment Window Reverse-Engineering Notes

## Target Version

- Executable: `DivaMegaMix.exe`
- Architecture: x86-64
- File size: 402,069,440 bytes
- Version: v1.04
- SHA-256: `6F4DF714C28515C93B3023E7D6DAFA9095DD8835F762D54DB1726FB8BC04504D`
- Investigation date: 2026-07-22

All static RVAs, AOB patterns, and patches in this document apply to this version. Revalidate the byte patterns after any game update.

## Findings

The game stores judgment windows in seconds. The default values are:

| Judgment window | Positive boundary | Negative boundary |
|---|---:|---:|
| COOL | +0.030 seconds | -0.030 seconds |
| FINE | +0.070 seconds | -0.070 seconds |
| SAFE | +0.100 seconds | -0.100 seconds |
| Outer window | +0.130 seconds | -0.130 seconds |

In milliseconds:

- COOL: ±30 ms
- FINE: ±70 ms
- SAFE: ±100 ms
- Outer window: ±130 ms

Judgment-result enum values:

| Value | Judgment |
|---:|---|
| 0 | COOL |
| 2 | FINE |
| 4 | SAFE |
| 6 | SAD |
| 8 | WORST |

Other result values and special-note branches also exist. Testing with standard single notes confirmed the four values above.

## Judgment Flow

Main judgment-producing function:

```text
DivaMegaMix.exe+26AFD0
```

It eventually calls the core classification function:

```text
DivaMegaMix.exe+26D2E0
```

The core logic loads the time difference between the current input and the note into `xmm1`, then compares it against the manager object's early and late boundaries in sequence. The simplified logic for a standard note is:

```cpp
if (delta <= cool_positive && delta >= cool_negative)
    result = COOL;
else if (delta <= fine_positive && delta >= fine_negative)
    result = FINE;
else if (delta <= safe_positive && delta >= safe_negative)
    result = SAFE;
else if (delta <= outer_positive && delta >= outer_negative)
    result = outer_grade;
else
    result = WORST;
```

Window fields in the manager object:

| Field offset | Meaning |
|---:|---|
| `+1326C` | COOL positive boundary |
| `+13270` | COOL negative boundary |
| `+13274` | FINE positive boundary |
| `+13278` | FINE negative boundary |
| `+1327C` | SAFE positive boundary |
| `+13280` | SAFE negative boundary |
| `+13284` | Outer positive boundary |
| `+13288` | Outer negative boundary |

The manager address changes with the song's runtime state, so an absolute address captured during one run is not reliable. The patch therefore uses the manager pointer held in `RCX` at runtime inside the judgment function.

## Combo and Result Object

The first combo address identified during the investigation was:

```text
DivaMegaMix.exe+12EEFEC
```

Register-state inspection showed that it corresponds to:

```asm
mov [rcx+3BC],edi
```

Here, `EDI` contains the new combo count. Relevant fields in the result object are:

| Object offset | Meaning |
|---:|---|
| `+3B0` | Judgment-result enum |
| `+3B4` | Timing difference for the current judgment |
| `+3BC` | Combo count |

Result-processing function:

```text
DivaMegaMix.exe+279390
```

The `mov [rcx+3BC],edi` instruction at `DivaMegaMix.exe+2793EE` only updates state after a judgment; it does not calculate the judgment window.

The nearby values `58.0`, `2.0`, and `0.025` relate to result display or state timing, not the judgment window.

## FINE Window Expansion Patch

Script file:

```text
C:\Users\justl\Downloads\diva_fine_window.lua
```

The patch hooks:

```text
DivaMegaMix.exe+26D31E
```

Original instruction:

```asm
movss xmm1,[rdx+18]
```

AOB pattern:

```text
F3 0F 10 4A 18 0F 2F 89 84 32 01 00
```

The code cave first writes the configured positive and negative FINE boundaries to `+13274/+13278` in the current manager. It then executes the original instruction and returns. The COOL, SAFE, and outer boundaries are not modified.

### Usage After Each Game Launch

1. Start the game.
2. Start the 64-bit version of Cheat Engine.
3. Open Cheat Engine's Lua Engine.
4. Run:

```lua
DIVA_FINE_WINDOW_MS=100; dofile([[C:\Users\justl\Downloads\diva_fine_window.lua]])
```

The script calls `openProcess('DivaMegaMix.exe')` itself, so you do not need to select the game process manually beforehand.

Success message:

```text
DIVA FINE window installed: +/- 100.000 ms (default was +/- 70 ms)
```

At ±100 ms, the 70–100 ms range that normally produces SAFE is classified as FINE instead. The FINE range therefore covers almost the entire SAFE range.

### Restoring the Default Behavior

Run the following in the same game process:

```lua
disableDivaFineWindow()
```

Alternatively, exit and restart the game. The patch exists only in the current process and does not modify `DivaMegaMix.exe` on disk.
