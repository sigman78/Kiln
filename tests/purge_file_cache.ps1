# Drops the Windows file cache pages of every file in a directory, without admin rights:
# opening a file unbuffered (FILE_FLAG_NO_BUFFERING) makes the cache manager purge its pages.
param([Parameter(Mandatory)] [string] $Dir)
$noBuffering = [System.IO.FileOptions] 0x20000000
$n = 0
Get-ChildItem -LiteralPath $Dir -File -Recurse | ForEach-Object {
    $f = [System.IO.FileStream]::new($_.FullName, [System.IO.FileMode]::Open, [System.IO.FileAccess]::Read,
                                     [System.IO.FileShare]::ReadWrite, 4096, $noBuffering)
    $f.Dispose()
    $n++
}
"purged $n files in $Dir"
