param(
    [Parameter(Mandatory = $true)][string]$Base,
    [Parameter(Mandatory = $true)][string]$Delta,
    [Parameter(Mandatory = $true)][string]$Output
)

$ErrorActionPreference = 'Stop'
$source = [System.IO.File]::ReadAllText($Base)
$oldSignature = 'std::string api_semantic_status(const std::filesystem::path& source)'
$newSignature = 'std::string api_semantic_status_full(const std::filesystem::path& source)'
$signatureIndex = $source.IndexOf($oldSignature, [System.StringComparison]::Ordinal)
if ($signatureIndex -lt 0) {
    throw 'Unable to find Atlas semantic status implementation'
}
$source = $source.Remove($signatureIndex, $oldSignature.Length).Insert($signatureIndex, $newSignature)

$anchor = 'std::filesystem::path semantic_map_path'
$anchorIndex = $source.IndexOf($anchor, [System.StringComparison]::Ordinal)
if ($anchorIndex -lt 0) {
    throw 'Unable to find Atlas semantic namespace anchor'
}
$declaration = "std::string api_semantic_status(const std::filesystem::path& source);`r`n`r`n"
$source = $source.Insert($anchorIndex, $declaration)

$deltaSource = [System.IO.File]::ReadAllText($Delta)
$combined = $source + "`r`n" + $deltaSource
[System.IO.File]::WriteAllText($Output, $combined, [System.Text.UTF8Encoding]::new($false))
