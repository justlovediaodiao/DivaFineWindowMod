-- Runtime FINE-window patch for Project DIVA Mega Mix+.
-- Set DIVA_FINE_WINDOW_MS before loading this file (default: 100 ms).

openProcess('DivaMegaMix.exe')
if not getAddressSafe('DivaMegaMix.exe') then
  error('DivaMegaMix.exe is not running')
end

local milliseconds = tonumber(DIVA_FINE_WINDOW_MS) or 100
if milliseconds < 30 or milliseconds > 130 then
  error('DIVA_FINE_WINDOW_MS must be between 30 and 130')
end

-- Re-running the script replaces the previous setting cleanly.
if divaFineHookScript and divaFineHookDisableInfo then
  local disabled, disableError = autoAssemble(
    divaFineHookScript, divaFineHookDisableInfo)
  if not disabled then
    error('Could not remove previous FINE-window hook: ' .. tostring(disableError))
  end
  divaFineHookScript = nil
  divaFineHookDisableInfo = nil
end

local seconds = milliseconds / 1000.0
local script = string.format([[
[ENABLE]
aobscanmodule(DIVA_FINE_HOOK,DivaMegaMix.exe,F3 0F 10 4A 18 0F 2F 89 84 32 01 00)
alloc(DIVA_FINE_CAVE,256,DIVA_FINE_HOOK)
label(DIVA_FINE_RETURN)
label(DIVA_FINE_POSITIVE)
label(DIVA_FINE_NEGATIVE)
registersymbol(DIVA_FINE_HOOK)

DIVA_FINE_CAVE:
  movss xmm1,[DIVA_FINE_POSITIVE]
  movss [rcx+00013274],xmm1
  movss xmm1,[DIVA_FINE_NEGATIVE]
  movss [rcx+00013278],xmm1
  movss xmm1,[rdx+18]
  jmp DIVA_FINE_RETURN

align 4
DIVA_FINE_POSITIVE:
  dd (float)%.9f
DIVA_FINE_NEGATIVE:
  dd (float)-%.9f

DIVA_FINE_HOOK:
  jmp DIVA_FINE_CAVE
DIVA_FINE_RETURN:

[DISABLE]
DIVA_FINE_HOOK:
  db F3 0F 10 4A 18
unregistersymbol(DIVA_FINE_HOOK)
dealloc(DIVA_FINE_CAVE)
]], seconds, seconds)

local ok, disableInfo = autoAssemble(script)
if not ok then error('FINE-window patch failed: ' .. tostring(disableInfo)) end

divaFineHookScript = script
divaFineHookDisableInfo = disableInfo

function disableDivaFineWindow()
  if not divaFineHookScript or not divaFineHookDisableInfo then
    print('DIVA FINE-window patch is not active.')
    return
  end
  local disabled, disableError = autoAssemble(
    divaFineHookScript, divaFineHookDisableInfo)
  if not disabled then error('Disable failed: ' .. tostring(disableError)) end
  divaFineHookScript = nil
  divaFineHookDisableInfo = nil
  print('DIVA FINE-window patch disabled; original code restored.')
end

print(string.format(
  'DIVA FINE window installed: +/- %.3f ms (default was +/- 70 ms)',
  milliseconds))
