# SavanXP IDE

This extension adds a SavanXP-focused explorer to VS Code for external SDK apps.

## Features

- Detects a SavanXP repository from the current workspace or from `savanxp.projectRoot`
- Builds and runs SDK examples through the existing PowerShell scripts
- Shows a side panel with:
  - `Examples`: the folders under `sdk/`
  - `SDK Snippets`: curated insertable snippets for the public SDK
- Opens `subsystems/posix/sdk/v1/REFERENCE.md` directly from the panel

## Load the extension

From the repository root:

```powershell
code --extensionDevelopmentPath .\tools\vscode-extension
```

Or open `tools/vscode-extension` in VS Code and launch the extension host with `F5`.

## Commands

- `SavanXP: New SDK App`
- `SavanXP: Build SDK App`
- `SavanXP: Build SDK App (No Install)`
- `SavanXP: Run SDK App`
- `SavanXP: Open SDK Reference`
- `SavanXP: Build Base OS Image`

## Notes

- The extension only targets external apps built against `subsystems/posix/sdk/v1`.
- It does not replace `clangd` or the VS Code C/C++ extension.
- Build and run commands reuse `tools/build-user.ps1`, `tools/new-user-app.ps1`, `tools/run-user.ps1` and `build.ps1`.
