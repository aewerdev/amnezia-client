sc stop AmneziaWGTunnel$AmneziaVPN
sc delete AmneziaWGTunnel$AmneziaVPN
taskkill /IM "AmneziaVPN-service.exe" /F
taskkill /IM "AmneziaVPN.exe" /F

set "AMNEZIA_BIN=%~dp0"
for %%I in ("%AMNEZIA_BIN%\.") do set "AMNEZIA_BIN=%%~fI"
powershell -NoProfile -ExecutionPolicy Bypass -Command "$entry=$env:AMNEZIA_BIN.TrimEnd('\'); $path=[Environment]::GetEnvironmentVariable('Path','Machine'); if ($null -eq $path) { $path='' }; $items=$path -split ';' | ForEach-Object { $_.Trim().TrimEnd('\') } | Where-Object { $_ }; if ($items -notcontains $entry) { $new=($path.TrimEnd(';') + ';' + $entry).Trim(';'); [Environment]::SetEnvironmentVariable('Path',$new,'Machine') }"
powershell -NoProfile -ExecutionPolicy Bypass -Command "$code='[DllImport(' + [char]34 + 'user32.dll' + [char]34 + ',SetLastError=true,CharSet=CharSet.Auto)] public static extern IntPtr SendMessageTimeout(IntPtr hWnd,uint Msg,UIntPtr wParam,string lParam,uint fuFlags,uint uTimeout,out UIntPtr lpdwResult);'; Add-Type -Namespace Win32 -Name NativeMethods -MemberDefinition $code; $result=[UIntPtr]::Zero; [Win32.NativeMethods]::SendMessageTimeout([IntPtr]0xffff,0x1A,[UIntPtr]::Zero,'Environment',2,5000,[ref]$result) | Out-Null"

exit /b 0
