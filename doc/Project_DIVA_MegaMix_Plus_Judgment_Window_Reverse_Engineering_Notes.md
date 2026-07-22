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


## Cheat Engine Patch

Script file:

```text
doc\diva_fine_window.cea
```

### FINE Window Hook

The script hooks:

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

### Leaderboard Upload Block

The script locates the
`CSteamLeaderboardRequest_Update` creation/enqueue function with an AOB scan and
replaces its first byte with `C3` (`ret`). This prevents a leaderboard update
request from being created. Disabling the script restores the original first
byte, `48`.

### Usage After Each Game Launch

1. Start the game.
2. Start the 64-bit version of Cheat Engine.
3. Attach Cheat Engine to `DivaMegaMix.exe`.
4. Open **Memory View**, then choose **Tools > Auto Assemble**.
5. In the Auto Assemble window, choose **File > Open** and open
   `doc\diva_fine_window.cea`.
6. Choose **File > Assign to current cheat table**, return to the main Cheat
   Engine window, and check the new Auto Assemble entry to enable it.

### Optional FINE Window Configuration

The default is ±100 ms. To change it, edit `DIVA_FINE_WINDOW_SECONDS` before
enabling the script. The value is in seconds and must be from `0.030` to
`0.130`:

```text
define(DIVA_FINE_WINDOW_SECONDS,0.100)
```

At ±100 ms, the 70–100 ms range that normally produces SAFE is classified as
FINE instead. To retain part of the SAFE range, use a value such as `0.080` or
`0.090`.

If the entry is already enabled, disable it before changing the value.

### Restoring the Default Behavior

Uncheck the Auto Assemble entry in Cheat Engine. This restores the original
FINE hook bytes and the leaderboard-upload function byte, then frees the code
cave. Alternatively, exit and restart the game.
