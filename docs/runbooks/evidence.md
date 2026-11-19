# Verify the reference evidence

Run from the project root in PowerShell 7.

```powershell
tar -tf docs/evidence/gameviewer-evidence.zip
$index = tar -xOf docs/evidence/gameviewer-evidence.zip index.json | ConvertFrom-Json
$index.rea | Select-Object entry, id, subject_sha256
```

Compare a supplied sample using `Get-FileHash -Algorithm SHA256`. The archive SHA-256 is `e0caecf15436ad8ba1038e5f2005b2b41c7756c199074336eb6bbd477cf73d2b`. The archive retains original binary identities and five REA 6.7.0 records. Distinguish file offsets from RVAs and static diagnostics from observed runtime events. For reproduction see the [analysis](../architecture/gameviewer-analysis.md).
