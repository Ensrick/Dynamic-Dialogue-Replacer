[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [ValidateScript({ Test-Path -LiteralPath $_ -PathType Leaf })]
    [string] $SkyrimExe,

    [Parameter(Mandatory)]
    [ValidateScript({ Test-Path -LiteralPath $_ -PathType Leaf })]
    [string] $AddressLibrary
)

$ErrorActionPreference = 'Stop'

$libraryBytes = [IO.File]::ReadAllBytes((Resolve-Path -LiteralPath $AddressLibrary))
if ($libraryBytes.Length -lt 96) {
    throw 'Address Library file is too short to contain a format-5 header.'
}

$format = [BitConverter]::ToInt32($libraryBytes, 0)
$version = (0..3 | ForEach-Object { [BitConverter]::ToUInt32($libraryBytes, 4 + ($_ * 4)) }) -join '.'
$imageName = [Text.Encoding]::ASCII.GetString($libraryBytes, 20, 64).Trim([char] 0)
$pointerSize = [BitConverter]::ToInt32($libraryBytes, 84)
$offsetCount = [BitConverter]::ToInt32($libraryBytes, 92)

if ($format -ne 5 -or $version -ne '1.7.104.0' -or $imageName -ne 'SkyrimSE.exe' -or $pointerSize -ne 8) {
    throw "Unexpected Address Library header: format=$format version=$version image=$imageName pointerSize=$pointerSize"
}

$requiredLibrarySize = 96L + (4L * $offsetCount)
if ($offsetCount -le 0 -or $libraryBytes.Length -lt $requiredLibrarySize) {
    throw "Address Library is truncated: bytes=$($libraryBytes.Length) required=$requiredLibrarySize"
}

$executablePath = (Resolve-Path -LiteralPath $SkyrimExe).Path
$executableVersion = (Get-Item -LiteralPath $executablePath).VersionInfo.FileVersion
if ([version] $executableVersion -ne [version] '1.7.104.0') {
    throw "Unexpected Skyrim executable version: $executableVersion"
}

$executableBytes = [IO.File]::ReadAllBytes($executablePath)
$peOffset = [BitConverter]::ToInt32($executableBytes, 0x3C)
if ([Text.Encoding]::ASCII.GetString($executableBytes, $peOffset, 4) -ne "PE$([char] 0)$([char] 0)") {
    throw 'Skyrim executable does not have a valid PE signature.'
}

$sectionCount = [BitConverter]::ToUInt16($executableBytes, $peOffset + 6)
$optionalHeaderSize = [BitConverter]::ToUInt16($executableBytes, $peOffset + 20)
$optionalHeaderOffset = $peOffset + 24
if ([BitConverter]::ToUInt16($executableBytes, $optionalHeaderOffset) -ne 0x20B) {
    throw 'Skyrim executable is not a PE32+ image.'
}
$imageBase = [BitConverter]::ToUInt64($executableBytes, $optionalHeaderOffset + 24)
$sectionTableOffset = $optionalHeaderOffset + $optionalHeaderSize
$sections = for ($index = 0; $index -lt $sectionCount; $index++) {
    $sectionOffset = $sectionTableOffset + (40 * $index)
    [pscustomobject] @{
        Name = [Text.Encoding]::ASCII.GetString($executableBytes, $sectionOffset, 8).Trim([char] 0)
        VirtualSize = [uint64] [BitConverter]::ToUInt32($executableBytes, $sectionOffset + 8)
        VirtualAddress = [uint64] [BitConverter]::ToUInt32($executableBytes, $sectionOffset + 12)
        RawSize = [uint64] [BitConverter]::ToUInt32($executableBytes, $sectionOffset + 16)
        RawOffset = [uint64] [BitConverter]::ToUInt32($executableBytes, $sectionOffset + 20)
    }
}

function Get-LibraryOffset {
    param([Parameter(Mandatory)][int] $Id)

    if ($Id -lt 0 -or $Id -ge $offsetCount) {
        throw "Address Library ID $Id is outside the dense table (count=$offsetCount)."
    }
    [BitConverter]::ToUInt32($libraryBytes, 96 + (4 * $Id))
}

function Resolve-Rva {
    param(
        [Parameter(Mandatory)][uint64] $Rva,
        [Parameter(Mandatory)][int] $ByteCount
    )

    $section = $sections | Where-Object {
        $Rva -ge $_.VirtualAddress -and
        ($Rva + $ByteCount) -le ($_.VirtualAddress + [Math]::Max($_.VirtualSize, $_.RawSize))
    } | Select-Object -First 1
    if (-not $section) {
        throw ('RVA 0x{0:X} is not contained in a PE section.' -f $Rva)
    }

    $fileOffset = $section.RawOffset + ($Rva - $section.VirtualAddress)
    if (($fileOffset + $ByteCount) -gt $executableBytes.Length) {
        throw ('RVA 0x{0:X} resolves beyond the executable.' -f $Rva)
    }

    [pscustomobject] @{
        Section = $section.Name
        FileOffset = $fileOffset
        Bytes = [byte[]] $executableBytes[$fileOffset..($fileOffset + $ByteCount - 1)]
    }
}

function Test-Bytes {
    param(
        [Parameter(Mandatory)][byte[]] $Actual,
        [Parameter(Mandatory)][byte[]] $Expected
    )

    if ($Actual.Length -lt $Expected.Length) {
        return $false
    }
    for ($index = 0; $index -lt $Expected.Length; $index++) {
        if ($Actual[$index] -ne $Expected[$index]) {
            return $false
        }
    }
    return $true
}

$hookSites = @(
    @(35249, 0x061, 'SetSubtitle'),
    @(35249, 0x0DE, 'ConstructResponse'),
    @(35287, 0x154, 'AddTopic.Primary'),
    @(35304, 0x06C, 'AddTopic.Secondary')
)

$results = foreach ($site in $hookSites) {
    $id = [int] $site[0]
    $siteRva = [uint64] (Get-LibraryOffset -Id $id) + [int] $site[1]
    $resolved = Resolve-Rva -Rva $siteRva -ByteCount 1
    [pscustomobject] @{
        Hook = [string] $site[2]
        Id = $id
        SiteRva = '0x{0:X}' -f $siteRva
        Section = $resolved.Section
        Opcode = '0x{0:X2}' -f $resolved.Bytes[0]
        Pass = $resolved.Section -eq '.text' -and $resolved.Bytes[0] -eq 0xE8
    }
}

$populateRva = [uint64] (Get-LibraryOffset -Id 35249)
$populate = Resolve-Rva -Rva $populateRva -ByteCount 8
$populateExpected = [byte[]] (0x48, 0x8B, 0xC4, 0x48, 0x89, 0x48, 0x08, 0x56)
if ($populate.Section -ne '.text' -or -not (Test-Bytes -Actual $populate.Bytes -Expected $populateExpected)) {
    throw 'PopulateTopicInfo does not match the audited 1.7.104 prologue.'
}

$addTopicRva = [uint64] (Get-LibraryOffset -Id 35303)
$addTopic = Resolve-Rva -Rva $addTopicRva -ByteCount 8
$addTopicExpected = [byte[]] (0x48, 0x8B, 0xC4, 0x55, 0x56, 0x57, 0x41, 0x54)
if ($addTopic.Section -ne '.text' -or -not (Test-Bytes -Actual $addTopic.Bytes -Expected $addTopicExpected)) {
    throw 'AddTopic does not match the audited 1.7.104 prologue.'
}

$dialogueMenuVtableRva = [uint64] (Get-LibraryOffset -Id 215255)
$vtable = Resolve-Rva -Rva $dialogueMenuVtableRva -ByteCount 40
$processMessageVa = [BitConverter]::ToUInt64($vtable.Bytes, 32)
if ($processMessageVa -lt $imageBase) {
    throw 'DialogueMenu::ProcessMessage vtable entry is below the executable image base.'
}
$processMessageRva = $processMessageVa - $imageBase
$processMessage = Resolve-Rva -Rva $processMessageRva -ByteCount 8
$processMessageExpected = [byte[]] (0x48, 0x8B, 0xC4, 0x55, 0x57, 0x41, 0x56, 0x48)
if ($vtable.Section -ne '.rdata' -or $processMessage.Section -ne '.text' -or
    -not (Test-Bytes -Actual $processMessage.Bytes -Expected $processMessageExpected)) {
    throw 'DialogueMenu::ProcessMessage does not match the audited 1.7.104 vtable entry.'
}

$results | Format-Table -AutoSize
$failed = @($results | Where-Object { -not $_.Pass })
if ($failed.Count -ne 0) {
    throw "$($failed.Count) hook site(s) do not begin with the expected E8 call opcode."
}

Write-Host 'PASS: format-5 Address Library, runtime metadata, four hook sites, two callable functions, and the DialogueMenu vtable were validated for Skyrim 1.7.104.0.'
