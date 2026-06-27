set AmneziaPath=%~dp0
echo %AmneziaPath%

rem Define directories for logs
set "ORG_DIR=%AppData%\AmneziaVPN.ORG"
set "USER_APP_DIR=%ORG_DIR%\AmneziaVPN"
set "USER_LOG_DIR=%USER_APP_DIR%\log"
set "SYS_APP_DIR=%ProgramData%\AmneziaVPN"
set "SYS_LOG_DIR=%SYS_APP_DIR%\log"
set "SYS_LOG_FILE=%SYS_LOG_DIR%\AmneziaVPN-service.log"

timeout /t 1
sc stop AmneziaVPN-service
sc delete AmneziaVPN-service
sc stop AmneziaWGTunnel$AmneziaVPN
sc delete AmneziaWGTunnel$AmneziaVPN
taskkill /IM "AmneziaVPN-service.exe" /F
taskkill /IM "AmneziaVPN.exe" /F

set "AMNEZIA_BIN=%~dp0"
for %%I in ("%AMNEZIA_BIN%\.") do set "AMNEZIA_BIN=%%~fI"
powershell -NoProfile -ExecutionPolicy Bypass -Command "$entry=$env:AMNEZIA_BIN.TrimEnd('\'); $path=[Environment]::GetEnvironmentVariable('Path','Machine'); if ($path) { $items=@($path -split ';' | Where-Object { $_.Trim() -and ($_.Trim().TrimEnd('\') -ne $entry) }); [Environment]::SetEnvironmentVariable('Path',($items -join ';'),'Machine') }"
powershell -NoProfile -ExecutionPolicy Bypass -Command "$code='[DllImport(' + [char]34 + 'user32.dll' + [char]34 + ',SetLastError=true,CharSet=CharSet.Auto)] public static extern IntPtr SendMessageTimeout(IntPtr hWnd,uint Msg,UIntPtr wParam,string lParam,uint fuFlags,uint uTimeout,out UIntPtr lpdwResult);'; Add-Type -Namespace Win32 -Name NativeMethods -MemberDefinition $code; $result=[UIntPtr]::Zero; [Win32.NativeMethods]::SendMessageTimeout([IntPtr]0xffff,0x1A,[UIntPtr]::Zero,'Environment',2,5000,[ref]$result) | Out-Null"

rem Delete the service log file under ProgramData
if exist "%SYS_LOG_FILE%" del /F /Q "%SYS_LOG_FILE%"
if exist "%SYS_LOG_DIR%" rmdir /S /Q "%SYS_LOG_DIR%"
rem Try to remove application dir if empty
rd "%SYS_APP_DIR%" 2>nul

rem Delete client logs under current user's AppData\Roaming (Organization\Application)
if exist "%USER_LOG_DIR%" rmdir /S /Q "%USER_LOG_DIR%"
rem Try to remove app and org directories if empty
rd "%USER_APP_DIR%" 2>nul
rd "%ORG_DIR%" 2>nul

exit /b 0
