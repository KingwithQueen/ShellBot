# Reports from older suites use status, while newer suites use passed.
# If both are present, neither is allowed to contradict a successful result.
function Test-ValidationReportPassed($Report) {
    $hasPassed = $null -ne $Report.PSObject.Properties['passed']
    $hasStatus = $null -ne $Report.PSObject.Properties['status']
    if (!$hasPassed -and !$hasStatus) { return $false }
    if ($hasPassed -and ($Report.passed -isnot [bool] -or $Report.passed -cne $true)) { return $false }
    if ($hasStatus -and $Report.status -cne 'passed') { return $false }
    return $true
}
