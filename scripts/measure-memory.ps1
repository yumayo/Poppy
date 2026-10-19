#requires -Version 5.1
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][int]$ProcessId,
    [ValidateRange(1, 3600)][int]$Samples = 30,
    [ValidateRange(100, 60000)][int]$IntervalMilliseconds = 1000,
    [switch]$IncludeChildren,
    [string]$CsvPath
)
$ErrorActionPreference = 'Stop'
Get-Process -Id $ProcessId | Out-Null
$records = for ($sample = 0; $sample -lt $Samples; $sample++) {
    if (-not (Get-Process -Id $ProcessId -ErrorAction SilentlyContinue)) { break }
    $ids = [System.Collections.Generic.HashSet[int]]::new()
    [void]$ids.Add($ProcessId)
    if ($IncludeChildren) {
        $snapshot = @(Get-CimInstance Win32_Process | Select-Object ProcessId, ParentProcessId)
        do {
            $added = $false
            foreach ($entry in $snapshot) {
                if ($ids.Contains([int]$entry.ParentProcessId) -and $ids.Add([int]$entry.ProcessId)) { $added = $true }
            }
        } while ($added)
    }
    $processes = @(Get-Process -Id @($ids) -ErrorAction SilentlyContinue)
    [PSCustomObject]@{
        Time = (Get-Date).ToString('o')
        Processes = $processes.Count
        WorkingSetMiB = [math]::Round((($processes | Measure-Object WorkingSet64 -Sum).Sum / 1MB), 2)
        PrivateMiB = [math]::Round((($processes | Measure-Object PrivateMemorySize64 -Sum).Sum / 1MB), 2)
    }
    if ($sample + 1 -lt $Samples) { Start-Sleep -Milliseconds $IntervalMilliseconds }
}
if (-not $records) { throw 'No samples collected.' }
if ($CsvPath) { $records | Export-Csv -Path $CsvPath -NoTypeInformation -Encoding UTF8 }
$records | Format-Table -AutoSize
$records | Measure-Object WorkingSetMiB, PrivateMiB -Average -Maximum | Format-Table Property, Average, Maximum -AutoSize
