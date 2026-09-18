param(
  [Parameter(Mandatory = $true)]
  [string]$RuntimeDir,
  [string]$ExpectedProtocolVersion = '0.8.0'
)

$ErrorActionPreference = 'Stop'
$RuntimeDir = [System.IO.Path]::GetFullPath($RuntimeDir)
$adapterPath = Join-Path $RuntimeDir 'MirrorSimAdapter.exe'
if (-not (Test-Path -LiteralPath $adapterPath -PathType Leaf)) {
  throw "Expected runtime output '$adapterPath' was not produced."
}

$startInfo = [System.Diagnostics.ProcessStartInfo]::new()
$startInfo.FileName = $adapterPath
$startInfo.WorkingDirectory = $RuntimeDir
$startInfo.UseShellExecute = $false
$startInfo.CreateNoWindow = $true
$startInfo.RedirectStandardInput = $true
$startInfo.RedirectStandardOutput = $true
$startInfo.RedirectStandardError = $true
$startInfo.EnvironmentVariables['MIRRORSIM_EXTERNAL_DNSSD'] = '1'
$startInfo.EnvironmentVariables['MIRRORSIM_HARDWARE_ADDRESS'] = '0210ABCDEF08'

$adapterProcess = [System.Diagnostics.Process]::new()
$adapterProcess.StartInfo = $startInfo
if (-not $adapterProcess.Start()) {
  throw 'MirrorSimAdapter smoke test could not start.'
}

try {
  $readyTask = $adapterProcess.StandardOutput.ReadLineAsync()
  if (-not $readyTask.Wait(5000)) {
    throw 'MirrorSimAdapter did not emit receiver_ready within five seconds.'
  }
  $ready = $readyTask.Result | ConvertFrom-Json
  if ($ready.name -ne 'receiver_ready') {
    throw "MirrorSimAdapter emitted '$($ready.name)' before receiver_ready."
  }
  if ($ready.protocol_version -ne $ExpectedProtocolVersion) {
    throw "Expected adapter protocol $ExpectedProtocolVersion, received '$($ready.protocol_version)'."
  }
  $requiredCapabilities = @(
    'pcm-audio',
    'sender-volume',
    'video-geometry',
    'video-sender-state',
    'external-dnssd'
  )
  foreach ($capability in $requiredCapabilities) {
    if ($ready.capabilities -notcontains $capability) {
      throw "MirrorSimAdapter did not advertise the required $capability capability."
    }
  }

  $adapterProcess.StandardInput.WriteLine('{"name":"start_session","session_id":"runtime-smoke-session","expected_stream_id":"runtime-smoke-stream","receiver_name":"MirrorSim Runtime Smoke","trusted_device_ids":[],"blocked_device_ids":[]}')
  $adapterProcess.StandardInput.Flush()

  $listenDeadline = [DateTime]::UtcNow.AddSeconds(8)
  $listeningPorts = @()
  do {
    if ($adapterProcess.HasExited) {
      $stderr = $adapterProcess.StandardError.ReadToEnd()
      throw "MirrorSimAdapter exited while starting its AirPlay listeners. $stderr"
    }
    $listeningPorts = @(
      Get-NetTCPConnection -State Listen -ErrorAction SilentlyContinue |
        Where-Object { $_.OwningProcess -eq $adapterProcess.Id -and $_.LocalPort -in @(5001, 7001) } |
        Select-Object -ExpandProperty LocalPort -Unique
    )
    if ($listeningPorts.Count -lt 2) {
      Start-Sleep -Milliseconds 100
    }
  } while ($listeningPorts.Count -lt 2 -and [DateTime]::UtcNow -lt $listenDeadline)

  if (5001 -notin $listeningPorts -or 7001 -notin $listeningPorts) {
    throw "MirrorSimAdapter did not listen on both AirPlay ports. Observed: $($listeningPorts -join ', ')."
  }

  $adapterProcess.StandardInput.WriteLine('{"name":"shutdown"}')
  $adapterProcess.StandardInput.Close()
  if (-not $adapterProcess.WaitForExit(10000)) {
    throw 'MirrorSimAdapter did not stop within ten seconds.'
  }
  if ($adapterProcess.ExitCode -ne 0) {
    $stderr = $adapterProcess.StandardError.ReadToEnd()
    throw "MirrorSimAdapter smoke test exited with code $($adapterProcess.ExitCode). $stderr"
  }
}
finally {
  if (-not $adapterProcess.HasExited) {
    $adapterProcess.Kill()
    $adapterProcess.WaitForExit()
  }
  $adapterProcess.Dispose()
}

Write-Host "Validated MirrorSimAdapter protocol $ExpectedProtocolVersion and listeners on ports 5001 and 7001."
