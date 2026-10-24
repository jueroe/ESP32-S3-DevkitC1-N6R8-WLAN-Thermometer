# Flasht das Thermometer und prueft den Kaltstart.
#
#   tools\flash-und-pruefen.cmd            Port automatisch suchen
#   tools\flash-und-pruefen.cmd COM11      Port vorgeben
#
# Das Skript arbeitet vollstaendig offline: ESP-IDF, Toolchain und die
# Firmware liegen lokal. Es schreibt ein Protokoll nach garage-flash.log.

param(
    [string]$Port = ""
)

$ErrorActionPreference = 'Stop'
$projectDir = Split-Path -Parent $PSScriptRoot
$logFile = Join-Path $projectDir 'garage-flash.log'
$idfProfile = 'C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1'
$bootSeconds = 75

function Write-Step($text) {
    Write-Host ""
    Write-Host "=== $text ===" -ForegroundColor Cyan
}

function Find-BoardPort {
    $ports = [System.IO.Ports.SerialPort]::GetPortNames()
    foreach ($candidate in $ports) {
        $device = Get-CimInstance Win32_PnPEntity -ErrorAction SilentlyContinue |
                  Where-Object { $_.Name -match [regex]::Escape("($candidate)") }
        if ($device -and $device.Name -match 'CH34|CP210|FTDI|Silicon Labs|USB-SERIAL') {
            return $candidate
        }
    }
    return $null
}

# --- Port -----------------------------------------------------------------
Write-Step "Schritt 1: seriellen Port suchen"
if (-not $Port) {
    $Port = Find-BoardPort
}
if (-not $Port) {
    Write-Host "Kein USB-Seriell-Adapter gefunden." -ForegroundColor Red
    Write-Host "Bitte pruefen: USB-Kabel am Anschluss 'UART' (nicht 'USB'), Geraet eingeschaltet."
    exit 1
}
Write-Host "Port: $Port"

# --- ESP-IDF --------------------------------------------------------------
Write-Step "Schritt 2: ESP-IDF aktivieren"
if (-not (Test-Path $idfProfile)) {
    Write-Host "Nicht gefunden: $idfProfile" -ForegroundColor Red
    exit 1
}
. $idfProfile | Out-Null

# --- Flashen --------------------------------------------------------------
Write-Step "Schritt 3: Firmware bauen und flashen (dauert etwa 1 Minute)"
& idf.py -p $Port flash 2>&1 | Tee-Object -FilePath $logFile
if ($LASTEXITCODE -ne 0) {
    Write-Host ""
    Write-Host "Erster Versuch fehlgeschlagen - zweiter Versuch mit 115200 Baud" -ForegroundColor Yellow
    Write-Host "(laengere USB-Kabel sind bei 460800 Baud manchmal zu schnell)"
    & idf.py -p $Port -b 115200 flash 2>&1 | Tee-Object -FilePath $logFile -Append
}
if ($LASTEXITCODE -ne 0) {
    Write-Host ""
    Write-Host "Flashen fehlgeschlagen (Code $LASTEXITCODE)." -ForegroundColor Red
    Write-Host "Haeufigste Ursachen:"
    Write-Host "  - der Port ist von einem anderen Programm belegt (VS-Code-Monitor,"
    Write-Host "    serielles Terminal) - dieses schliessen und erneut starten"
    Write-Host "  - USB-Kabel zu lang oder ohne Datenadern - kuerzeres Kabel probieren"
    Write-Host "  - BOOT-Taster halten und dabei RESET druecken, dann Skript erneut starten"
    exit $LASTEXITCODE
}

# --- Kaltstart beobachten -------------------------------------------------
Write-Step "Schritt 4: Neustart und WLAN-Verbindung pruefen ($bootSeconds s)"
Start-Sleep -Seconds 2

$lines = New-Object System.Collections.Generic.List[string]
$serial = New-Object System.IO.Ports.SerialPort $Port, 115200, 'None', 8, 'One'
$serial.ReadTimeout = 500
$serial.DtrEnable = $false
$serial.RtsEnable = $false
$serial.Open()
Start-Sleep -Milliseconds 300

# kurzer Reset ueber die Steuerleitung, damit der Start ab dem ersten Zeichen
# protokolliert wird
$serial.RtsEnable = $true
Start-Sleep -Milliseconds 150
$serial.RtsEnable = $false

$start = Get-Date
$spin = '|', '/', '-', '\'
$i = 0
while (((Get-Date) - $start).TotalSeconds -lt $bootSeconds) {
    try {
        $line = $serial.ReadLine()
        $lines.Add($line)
        if ($line -match '^(I|W|E) \(') { Write-Host $line }
    } catch [TimeoutException] {
        $i++
        Write-Host ("`r" + $spin[$i % 4] + " warte auf Ausgaben ... ") -NoNewline
    }
}
$serial.Close()
Write-Host ""
$lines | Add-Content -Path $logFile

# --- Auswertung -----------------------------------------------------------
$text = $lines -join "`n"
function Test-Pattern($pattern) { return [bool]($text -match $pattern) }

Write-Step "Ergebnis"

$resetOk = Test-Pattern 'reset: power on|reset: software restart|reset: external reset'
$sensorOk = Test-Pattern 'SHT35 ready, first reading'
$sensorBad = Test-Pattern 'no SHT35 at|no I2C device answers|sensor lamp: on'
$scanRssi = if ($text -match 'found with (-?\d+) dBm') { [int]$Matches[1] }
            elseif ($text -match 'is weak \((-?\d+) dBm') { [int]$Matches[1] }
            else { $null }
$notInScan = Test-Pattern 'is not in the scan result'
$weak = Test-Pattern 'is weak \(|is not in the scan result'
$connected = Test-Pattern 'got IP'
$apMode = Test-Pattern 'no network - opening the configuration access point'
$announced = if ($text -match 'announced as http://([a-z0-9\-\.]+)') { $Matches[1] } else { $null }
$ip = if ($text -match 'got IP (\d+\.\d+\.\d+\.\d+)') { $Matches[1] } else { $null }
$linkRssi = if ($text -match 'link to .*RSSI (-?\d+) dBm') { [int]$Matches[1] } else { $null }
$watchdog = Test-Pattern 'task watchdog: \d+ s'
$rebootLoop = Test-Pattern 'reset: task watchdog|reset: PANIC|rst:0xc'

Write-Host ""
Write-Host ("Reset beim Start ....... {0}" -f $(if ($resetOk) { "ja" } else { "unklar" }))
Write-Host ("Watchdog ............... {0}" -f $(if ($watchdog) { "aktiv (60 s)" } else { "NICHT aktiv - falsche Firmware?" }))
Write-Host ("Sensor ................. {0}" -f $(if ($sensorOk -and -not $sensorBad) { "OK" } elseif ($sensorBad) { "FEHLER (rote LED)" } else { "keine Meldung" }))
if ($scanRssi) {
    Write-Host ("WLAN im Scan .......... {0} dBm{1}" -f $scanRssi, $(if ($weak) { " (schwach, wird trotzdem versucht)" } else { "" }))
} elseif ($notInScan) {
    Write-Host "WLAN im Scan .......... nicht gefunden (Verbindung wird trotzdem versucht)"
} else {
    Write-Host "WLAN im Scan .......... keine Meldung"
}
Write-Host ("Verbindung ............. {0}" -f $(if ($connected) { "OK" } else { "FEHLGESCHLAGEN" }))
if ($ip) { Write-Host ("IP-Adresse ............. {0}" -f $ip) }
if ($linkRssi) { Write-Host ("Pegel der Verbindung ... {0} dBm" -f $linkRssi) }
if ($announced) { Write-Host ("Name im Netz .......... http://{0}/" -f $announced) }

Write-Host ""
if ($connected -and -not $apMode) {
    Write-Host "ERFOLG: Das Geraet ist an dieser Position im WLAN." -ForegroundColor Green
    Write-Host "Die gruene LED muss jetzt dauerhaft leuchten."
    Write-Host ("Am Heimnetz-PC erreichbar als http://{0}/api/values" -f $(if ($announced) { $announced } else { "aussenthermometer.local" }))
} else {
    Write-Host "KEIN WLAN an dieser Position." -ForegroundColor Yellow
    Write-Host "Das Geraet hat den Konfigurations-Access Point geoeffnet (gruene LED:"
    Write-Host "2x blinken, dann 1 s Pause). Es startet nach 15 Minuten ohne Client"
    Write-Host "erneut und versucht es wieder - es bleibt also nicht haengen."
    Write-Host ""
    Write-Host "Moegliche Abhilfe: Geraet naeher zur Hauswand/zum Fenster, Repeater"
    Write-Host "in der Naehe, oder Modul mit besserer Antenne."
}

if ($rebootLoop) {
    Write-Host ""
    Write-Host "Hinweis: Im Protokoll steht ein Neustart durch Watchdog/Panic." -ForegroundColor Yellow
    Write-Host "Das Protokoll liegt in garage-flash.log - bitte dem Entwickler zeigen."
}

Write-Host ""
Write-Host "Protokoll gespeichert: $logFile"
