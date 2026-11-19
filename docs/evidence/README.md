# Evidence inventory

- [Native Windows preview](native-windows-preview.md) records the Qt/D3D11 viewer interop tests, FFmpeg path, and limits of the WinUI comparison.

[GameViewer evidence archive](gameviewer-evidence.zip) contains the Windows 4.48.0.5446 reference snapshot. Its manifest, `index.json`, lists 20 sample identities and five REA records. The original archive is preserved byte for byte.

The manifest identifies NetEase UU Remote for Windows and records a read-only snapshot of services, drivers, process relationships, and modules loaded by the main UI. No remote control session was initiated during reference analysis. File hashes, versions, and Windows observations were collected with PowerShell. Only the five raw JSON files under `rea/` are REA records. Offsets and relative virtual addresses refer to the exact sampled build.

Use the [verification runbook](../runbooks/evidence.md) to check a record and the [binary analysis](../architecture/gameviewer-analysis.md) to interpret it. Implementation qualification uses the [Windows session runbook](../runbooks/windows-session.md).

The [Windows v1 validation record](windows-v1-validation.md) reports the independently implemented endpoint's measured local results and outstanding field checks.
