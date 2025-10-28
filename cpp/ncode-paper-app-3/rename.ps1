# PowerShell script to rename mupdfSample project to ncode-paper-app-3
# Run this from the project directory

$oldName = "mupdfSample"
$newName = "ncode-paper-app-3"

Write-Host "Renaming Visual Studio project files from '$oldName' to '$newName'..." -ForegroundColor Cyan
Write-Host ""

# Define file mappings
$fileMappings = @{
    "$oldName.sln" = "$newName.sln"
    "$oldName.vcxproj" = "$newName.vcxproj"
    "$oldName.vcxproj.filters" = "$newName.vcxproj.filters"
    "$oldName.vcxproj.user" = "$newName.vcxproj.user"
}

# Check if files exist
$allFilesExist = $true
foreach ($oldFile in $fileMappings.Keys) {
    if (-not (Test-Path $oldFile)) {
        Write-Host "ERROR: File not found: $oldFile" -ForegroundColor Red
        $allFilesExist = $false
    }
}

if (-not $allFilesExist) {
    Write-Host "`nPlease run this script from the project directory containing the Visual Studio files." -ForegroundColor Yellow
    exit 1
}

# Rename files and update their contents
foreach ($oldFile in $fileMappings.Keys) {
    $newFile = $fileMappings[$oldFile]
    
    Write-Host "Processing: $oldFile -> $newFile" -ForegroundColor Green
    
    # Read file content
    $content = Get-Content $oldFile -Raw -Encoding UTF8
    
    # Replace all occurrences of old name with new name
    $content = $content -replace [regex]::Escape($oldName), $newName
    
    # Write to new file
    Set-Content -Path $newFile -Value $content -Encoding UTF8 -NoNewline
    
    # Remove old file
    Remove-Item $oldFile -Force
    
    Write-Host "  ✓ Renamed and updated" -ForegroundColor Gray
}

Write-Host ""
Write-Host "Project renamed successfully!" -ForegroundColor Green
Write-Host ""
Write-Host "Next steps:" -ForegroundColor Cyan
Write-Host "1. Open '$newName.sln' in Visual Studio"
Write-Host "2. Build the project to verify everything works"
Write-Host ""