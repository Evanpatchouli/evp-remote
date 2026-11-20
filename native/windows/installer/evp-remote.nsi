Unicode true
RequestExecutionLevel user
ManifestDPIAware true
CRCCheck force
SetCompressor /SOLID lzma

!include MUI2.nsh
!include x64.nsh
!include FileFunc.nsh

!ifndef INSTALL_NAME
  !define INSTALL_NAME "EVP Remote"
!endif
!ifndef INSTALL_ID
  !define INSTALL_ID "evp-remote"
!endif
!define PRODUCT "EVP Remote"
!define PUBLISHER "Evanpatchouli"
!define WEBSITE "https://github.com/Evanpatchouli/evp-remote"
!define UNINSTALL_KEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\${INSTALL_ID}"
!define INSTALL_PATH "$LOCALAPPDATA\Programs\${INSTALL_NAME}"
!define PENDING_VERSION "PendingVersion"
!define OWNERSHIP_FILE ".evp-remote-install"
!define SHORTCUT "$SMPROGRAMS\${INSTALL_NAME}.lnk"

Name "${PRODUCT}"
OutFile "${OUTPUT_EXE}"
; A trailing separator keeps NSIS Browse from appending the product folder.
InstallDir "${INSTALL_PATH}\"
VIProductVersion "${NUMERIC_VERSION}"
VIFileVersion "${NUMERIC_VERSION}"

!define MUI_ICON "${APP_ICON}"
!define MUI_UNICON "${APP_ICON}"
!define MUI_ABORTWARNING
!define MUI_FINISHPAGE_RUN "$INSTDIR\EvpRemote.exe"
!define MUI_FINISHPAGE_RUN_NOTCHECKED
!define MUI_FINISHPAGE_NOREBOOTSUPPORT
!insertmacro MUI_PAGE_WELCOME
!define MUI_PAGE_CUSTOMFUNCTION_LEAVE ConfirmDestination
!define MUI_DIRECTORYPAGE_TEXT_TOP "$(ChooseDestination)"
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "English"
!insertmacro MUI_LANGUAGE "SimpChinese"

!macro VersionInfo LANGUAGE
  VIAddVersionKey /LANG=${LANGUAGE} "ProductName" "${PRODUCT}"
  VIAddVersionKey /LANG=${LANGUAGE} "FileDescription" "${PRODUCT} Setup"
  VIAddVersionKey /LANG=${LANGUAGE} "CompanyName" "${PUBLISHER}"
  VIAddVersionKey /LANG=${LANGUAGE} "ProductVersion" "${VERSION}"
  VIAddVersionKey /LANG=${LANGUAGE} "FileVersion" "${VERSION}"
  VIAddVersionKey /LANG=${LANGUAGE} "LegalCopyright" "See LICENSE and third-party notices."
!macroend
!insertmacro VersionInfo ${LANG_ENGLISH}
!insertmacro VersionInfo ${LANG_SIMPCHINESE}

LangString UnsupportedWindows ${LANG_ENGLISH} "EVP Remote requires 64-bit Windows."
LangString UnsupportedWindows ${LANG_SIMPCHINESE} "EVP 远程需要 64 位 Windows。"
LangString InstallBusy ${LANG_ENGLISH} "Another EVP Remote setup or uninstall is running. Try again after it closes."
LangString InstallBusy ${LANG_SIMPCHINESE} "EVP 远程正在执行另一项安装或卸载。请在它关闭后重试。"
LangString UnmanagedFolder ${LANG_ENGLISH} "Setup could not verify the EVP Remote installation folder:$\n$INSTDIR"
LangString UnmanagedFolder ${LANG_SIMPCHINESE} "安装程序无法验证 EVP 远程的安装目录：$\n$INSTDIR"
LangString InstallFailed ${LANG_ENGLISH} "Setup could not install EVP Remote. Close any running EVP Remote windows and try again."
LangString InstallFailed ${LANG_SIMPCHINESE} "无法安装 EVP 远程。请关闭正在运行的 EVP 远程窗口后重试。"
LangString RestoreFailed ${LANG_ENGLISH} "Setup could not restore the previous EVP Remote installation. Its files are at:$\n$BackupDir"
LangString RestoreFailed ${LANG_SIMPCHINESE} "安装程序无法恢复之前的 EVP 远程安装。安装文件位于：$\n$BackupDir"
LangString UninstallFailed ${LANG_ENGLISH} "EVP Remote could not be uninstalled. Close any running EVP Remote windows and try again."
LangString UninstallFailed ${LANG_SIMPCHINESE} "无法卸载 EVP 远程。请关闭正在运行的 EVP 远程窗口后重试。"
LangString PendingUpdate ${LANG_ENGLISH} "An EVP Remote update is pending. Wait for it to finish. If it was interrupted before startup was confirmed, close EVP Remote and run setup with /S /UPDATEROLLBACK. If startup was confirmed, run setup with /S /UPDATECOMMIT."
LangString PendingUpdate ${LANG_SIMPCHINESE} "请等待 EVP 远程更新完成。如果更新在确认启动前中断，请关闭 EVP 远程，然后使用 /S /UPDATEROLLBACK 运行安装程序。如果已确认启动，请使用 /S /UPDATECOMMIT 完成更新。"
LangString ChooseDestination ${LANG_ENGLISH} "Choose where to install EVP Remote. To change locations, select a new or empty folder. The previous folder will be left in place. Manage the active installation through Windows Settings > Apps."
LangString ChooseDestination ${LANG_SIMPCHINESE} "请选择 EVP 远程的安装目录。更改位置时，请选择新文件夹或空文件夹。旧文件夹会保留，请在 Windows 设置的应用页面管理当前安装。"
LangString InvalidDestination ${LANG_ENGLISH} "Choose a valid folder on a local drive. The folder must be empty or already registered for EVP Remote. It cannot be inside or contain the previous installation folder."
LangString InvalidDestination ${LANG_SIMPCHINESE} "请选择本地磁盘上的有效文件夹。文件夹须为空或已登记为 EVP 远程安装目录，且新旧安装目录不能相互包含。"

Var StageDir
Var BackupDir
Var HadPrevious
Var FailureMessage
Var UpdateMode
Var PreviousVersion
Var PendingStarted
Var DiscardDir
Var RegisteredDir
Var InstallParent
Var UpdateBackup
Var Relocating
Var ShortcutStage
Var RegisteredVersion

!macro Initialize
  ${IfNot} ${RunningX64}
    MessageBox MB_ICONSTOP "$(UnsupportedWindows)" /SD IDOK
    SetErrorLevel 1
    Abort
  ${EndIf}
  SetShellVarContext current
  SetRegView 64
  System::Call 'kernel32::CreateMutexW(p 0, i 0, w "Local\${INSTALL_ID}-setup") p .r0 ?e'
  Pop $1
  ${If} $0 = 0
  ${OrIf} $1 = 183
    MessageBox MB_ICONSTOP "$(InstallBusy)" /SD IDOK
    SetErrorLevel 1
    Abort
  ${EndIf}
  ; Windows releases the mutex handle when setup exits.
!macroend

Function .onInit
  !insertmacro Initialize
  ReadRegStr $RegisteredDir HKCU "${UNINSTALL_KEY}" "InstallLocation"
  ${If} $RegisteredDir != ""
    StrCpy $INSTDIR "$RegisteredDir"
  ${Else}
    ; NSIS can discard an invalid /D path before exposing $INSTDIR. Validate
    ; the explicit path rather than installing to the default as a fallback.
    System::Call 'kernel32::GetCommandLineW() w .r0'
    ${GetOptionsS} $0 "/D=" $1
    ${IfNot} ${Errors}
      StrCpy $INSTDIR "$1"
    ${EndIf}
    StrLen $0 $INSTDIR
    StrCpy $1 $INSTDIR 1 -1
    ${If} $0 > 3
    ${AndIf} $1 == "\"
      StrCpy $INSTDIR $INSTDIR -1
    ${EndIf}
  ${EndIf}
  ${GetParameters} $0
  ${GetOptions} $0 "/UPDATEBACKUP" $1
  ${IfNot} ${Errors}
    StrCpy $UpdateMode "backup"
  ${EndIf}
  ${GetOptions} $0 "/UPDATECOMMIT" $1
  ${IfNot} ${Errors}
    StrCpy $UpdateMode "commit"
  ${EndIf}
  ${GetOptions} $0 "/UPDATEROLLBACK" $1
  ${IfNot} ${Errors}
    StrCpy $UpdateMode "rollback"
  ${EndIf}
  ${If} $UpdateMode != ""
    IfSilent +3
    SetErrorLevel 1
    Abort
  ${EndIf}
FunctionEnd

Function un.onInit
  !insertmacro Initialize
  ReadRegStr $RegisteredDir HKCU "${UNINSTALL_KEY}" "InstallLocation"
FunctionEnd

; Never follow directory links when removing a previous installation.
!macro RemoveTree PREFIX
Function ${PREFIX}RemoveTree
  Exch $0
  Push $1
  Push $2
  Push $3
  System::Call 'kernel32::GetFileAttributesW(w r0) i .r3'
  ${If} $3 <> -1
    IntOp $3 $3 & 0x410
    ${If} $3 = 0x10
      FindFirst $1 $2 "$0\*"
      ${While} $2 != ""
        ${If} $2 != "."
        ${AndIf} $2 != ".."
          Push "$0\$2"
          Call ${PREFIX}RemoveTree
        ${EndIf}
        FindNext $1 $2
      ${EndWhile}
      FindClose $1
    ${EndIf}
    IntOp $3 $3 & 0x10
    ${If} $3 <> 0
      RMDir "$0"
    ${Else}
      Delete "$0"
    ${EndIf}
  ${EndIf}
  Pop $3
  Pop $2
  Pop $1
  Pop $0
FunctionEnd
!macroend

; Require a canonical local path and reject links or aliases at every existing
; ancestor. A missing directory is valid and can be created during setup.
!macro ValidatePath PREFIX
Function ${PREFIX}ValidatePath
  Exch $0
  Push $1
  Push $2
  Push $3
  Push $4
  ClearErrors
  StrCpy $1 $0 2 1
  StrCmp $1 ":\" 0 invalid
  StrCpy $1 $0 3
  StrCmp $1 $0 invalid
  StrCpy $2 $0 1 -1
  StrCmp $2 "\" invalid
  System::Call 'kernel32::GetDriveTypeW(w r1) i .r2'
  ${If} $2 <> 2
  ${AndIf} $2 <> 3
    Goto invalid
  ${EndIf}
  StrCpy $2 $0 "" 3
  StrCpy $3 '/:*?<>|$\"'
  System::Call 'shlwapi::StrPBrkW(w r2, w r3) p .r4'
  StrCmp $4 0 0 invalid
  System::Call 'kernel32::GetFullPathNameW(w r0, i ${NSIS_MAX_STRLEN}, w .r2, p 0) i .r3'
  ${If} $3 = 0
  ${OrIf} $3 >= ${NSIS_MAX_STRLEN}
    Goto invalid
  ${EndIf}
  StrCmp $0 $2 0 invalid

ancestor:
  StrCpy $2 $0 1 -1
  StrCmp $2 " " invalid
  StrCmp $2 "." invalid
  System::Call 'kernel32::GetFileAttributesW(w r0) i .r2 ?e'
  Pop $3
  ${If} $2 = -1
    ${If} $3 <> 2
    ${AndIf} $3 <> 3
      Goto invalid
    ${EndIf}
  ${Else}
    IntOp $2 $2 & 0x410
    StrCmp $2 16 0 invalid
    System::Call 'kernel32::CreateFileW(w r0, i 0x80, i 7, p 0, i 3, i 0x02200000, p 0) p .r4'
    StrCmp $4 -1 invalid
    System::Call 'kernel32::GetFinalPathNameByHandleW(p r4, w .r2, i ${NSIS_MAX_STRLEN}, i 0) i .r3'
    System::Call 'kernel32::CloseHandle(p r4)'
    ${If} $3 = 0
    ${OrIf} $3 >= ${NSIS_MAX_STRLEN}
      Goto invalid
    ${EndIf}
    StrCpy $2 $2 "" 4
    StrCmp $2 $0 0 invalid
  ${EndIf}
  StrCmp $0 $1 valid
  ${GetParent} $0 $0
  StrLen $2 $0
  ${If} $2 = 2
    StrCpy $0 "$0\"
  ${EndIf}
  StrCmp $0 "" invalid ancestor

valid:
  ClearErrors
  Goto done
invalid:
  SetErrors
done:
  Pop $4
  Pop $3
  Pop $2
  Pop $1
  Pop $0
FunctionEnd
!macroend
!insertmacro ValidatePath ""
!insertmacro ValidatePath "un."

!macro CheckOwnedFolder PATH FAIL
  System::Call 'kernel32::GetFileAttributesW(w "${PATH}") i .r0'
  IntOp $0 $0 & 0x410
  ${If} $0 <> 0x10
    Goto ${FAIL}
  ${EndIf}
  ClearErrors
  FileOpen $0 "${PATH}\${OWNERSHIP_FILE}" r
  IfErrors ${FAIL}
  FileRead $0 $1
  FileClose $0
  IfErrors ${FAIL}
  ${If} $1 != "${INSTALL_ID}$\r$\n"
    Goto ${FAIL}
  ${EndIf}
!macroend

!macro DestinationParent
  ${GetParent} $INSTDIR $InstallParent
  StrCpy $UpdateBackup "$InstallParent\.${INSTALL_ID}-update-backup"
  StrLen $0 $InstallParent
  ${If} $0 = 2
    StrCpy $InstallParent "$InstallParent\"
  ${ElseIf} $0 = 3
    StrCpy $UpdateBackup "$InstallParent.${INSTALL_ID}-update-backup"
  ${EndIf}
!macroend

Function CheckDestination
  StrCpy $FailureMessage "$(InvalidDestination)"
  StrCpy $Relocating 0
  Push "$INSTDIR"
  Call ValidatePath
  IfErrors invalid
  !insertmacro DestinationParent
  ${If} $INSTDIR == $UpdateBackup
    Goto invalid
  ${EndIf}
  ${If} $RegisteredDir != ""
    ReadRegStr $RegisteredVersion HKCU "${UNINSTALL_KEY}" "DisplayVersion"
    IfErrors invalid
  ${EndIf}
  ${If} $RegisteredDir != ""
  ${AndIf} $RegisteredDir != $INSTDIR
    Push "$RegisteredDir"
    Call ValidatePath
    IfErrors invalid
    !insertmacro CheckOwnedFolder "$RegisteredDir" invalid
    StrLen $0 "$RegisteredDir\"
    StrCpy $1 "$INSTDIR\" $0
    StrCmp $1 "$RegisteredDir\" invalid
    StrLen $0 "$INSTDIR\"
    StrCpy $1 "$RegisteredDir\" $0
    StrCmp $1 "$INSTDIR\" invalid
    ${GetParent} $RegisteredDir $0
    ReadRegStr $1 HKCU "${UNINSTALL_KEY}" "${PENDING_VERSION}"
    StrCmp $1 "" 0 pending
    IfFileExists "$0\.${INSTALL_ID}-update-backup" pending
    ClearErrors
    ; An open runtime image prevents safe relocation of the active registration.
    System::Call 'kernel32::CreateFileW(w "$RegisteredDir\EvpRemote.exe", i 0x10000, i 0, p 0, i 3, i 0x00200000, p 0) p .r0'
    StrCmp $0 -1 busy
    System::Call 'kernel32::CloseHandle(p r0)'
    StrCpy $Relocating 1
  ${EndIf}
  IfFileExists "$INSTDIR" 0 valid
  ${If} $INSTDIR == $RegisteredDir
    !insertmacro CheckOwnedFolder "$INSTDIR" invalid
    Goto valid
  ${EndIf}
  ; The earlier attribute check established that this is a normal directory.
  FindFirst $0 $1 "$INSTDIR\*"
  IfErrors invalid
  ${While} $1 != ""
    ${If} $1 != "."
    ${AndIf} $1 != ".."
      FindClose $0
      Goto invalid
    ${EndIf}
    FindNext $0 $1
  ${EndWhile}
  FindClose $0
valid:
  ClearErrors
  Return
pending:
  StrCpy $FailureMessage "$(PendingUpdate)"
  Goto invalid
busy:
  StrCpy $FailureMessage "$(InstallFailed)"
invalid:
  SetErrors
FunctionEnd

Function ConfirmDestination
  Call CheckDestination
  ${If} ${Errors}
    MessageBox MB_ICONSTOP "$FailureMessage"
    Abort
  ${EndIf}
FunctionEnd
!insertmacro RemoveTree ""
!insertmacro RemoveTree "un."

; The 0.2.0 uninstaller can unregister whatever is installed at the time it
; runs. Refresh only that generated file before publishing a new location.
; The refreshed uninstaller still works at the old path if relocation fails.
Function GuardPreviousUninstaller
  IfFileExists "$RegisteredDir\Uninstall.exe" 0 done
  System::Call 'kernel32::GetFileAttributesW(w "$RegisteredDir\Uninstall.exe") i .r0'
  IntOp $0 $0 & 0x410
  StrCmp $0 0 0 failed
  GetTempFileName $1 "$RegisteredDir"
  IfErrors failed
  System::Call 'kernel32::CopyFileW(w "$INSTDIR\Uninstall.exe", w r1, i 0) i .r0'
  StrCmp $0 0 discard
  System::Call 'kernel32::MoveFileExW(w r1, w "$RegisteredDir\Uninstall.exe", i 9) i .r0'
  StrCmp $0 0 discard
done:
  ClearErrors
  Return
discard:
  Delete "$1"
failed:
  SetErrors
FunctionEnd

!macro RegisterPath PATH
  WriteRegStr HKCU "${UNINSTALL_KEY}" "DisplayIcon" "${PATH}\EvpRemote.exe,0"
  WriteRegStr HKCU "${UNINSTALL_KEY}" "InstallLocation" "${PATH}"
  WriteRegStr HKCU "${UNINSTALL_KEY}" "UninstallString" '$\"${PATH}\Uninstall.exe$\"'
  WriteRegStr HKCU "${UNINSTALL_KEY}" "QuietUninstallString" '$\"${PATH}\Uninstall.exe$\" /S'
!macroend

; Commit and rollback use the same guarded paths as ordinary setup. The backup
; exists until the update helper confirms that the replacement started.
Function FinishUpdate
  StrCpy $BackupDir "$UpdateBackup"
  ${If} $PreviousVersion == ""
  ${AndIf} $UpdateMode == "rollback"
    Goto failed
  ${EndIf}
  ReadRegStr $2 HKCU "${UNINSTALL_KEY}" "DisplayVersion"
  IfErrors failed
  IfFileExists "$BackupDir" 0 no_backup
  !insertmacro CheckOwnedFolder "$BackupDir" failed
  IfFileExists "$INSTDIR" 0 no_active
  !insertmacro CheckOwnedFolder "$INSTDIR" failed

  GetTempFileName $DiscardDir "$InstallParent"
  IfErrors failed
  Delete "$DiscardDir"
  IfErrors failed
  ${If} $UpdateMode == "commit"
    StrCmpS $2 "${VERSION}" 0 failed
    ; Clear the pending marker before retiring the backup so an interrupted
    ; commit can find the still-owned backup at its fixed path and retry.
    ${If} $PreviousVersion != ""
      DeleteRegValue HKCU "${UNINSTALL_KEY}" "${PENDING_VERSION}"
      IfErrors failed
    ${EndIf}
    Rename "$BackupDir" "$DiscardDir"
    ${If} ${Errors}
      ${If} $PreviousVersion != ""
        WriteRegStr HKCU "${UNINSTALL_KEY}" "${PENDING_VERSION}" "$PreviousVersion"
      ${EndIf}
      Goto failed
    ${EndIf}
    Goto cleanup
  ${EndIf}

  Rename "$INSTDIR" "$DiscardDir"
  IfErrors failed

no_active:
  StrCmp $UpdateMode "rollback" 0 failed
  ; Write the prior version before restoring its directory so an interrupted
  ; rollback can finish idempotently once the backup has moved back.
  WriteRegStr HKCU "${UNINSTALL_KEY}" "DisplayVersion" "$PreviousVersion"
  IfErrors restore_active
  Rename "$BackupDir" "$INSTDIR"
  IfErrors restore_metadata
  Goto clear_pending

no_backup:
  ${If} $UpdateMode == "commit"
    StrCmpS $2 "${VERSION}" 0 failed
    StrCmp $PreviousVersion "" 0 failed
    !insertmacro CheckOwnedFolder "$INSTDIR" failed
    Goto cleanup
  ${EndIf}
  ; The old directory can still be in place if setup was interrupted before
  ; the initial rename or after rollback already restored it.
  StrCmp $UpdateMode "rollback" 0 failed
  StrCmpS $2 $PreviousVersion 0 failed
  !insertmacro CheckOwnedFolder "$INSTDIR" failed

clear_pending:
  DeleteRegValue HKCU "${UNINSTALL_KEY}" "${PENDING_VERSION}"
  IfErrors failed
  Goto cleanup

restore_metadata:
  WriteRegStr HKCU "${UNINSTALL_KEY}" "DisplayVersion" "$2"
restore_active:
  ${If} $DiscardDir != ""
    Rename "$DiscardDir" "$INSTDIR"
  ${EndIf}
  Goto failed

cleanup:
  ${If} $DiscardDir != ""
    Push $DiscardDir
    Call RemoveTree
  ${EndIf}
  ClearErrors
  Return

failed:
  SetErrors
FunctionEnd

Section
  Call CheckDestination
  IfErrors failed
  StrCpy $FailureMessage "$(InstallFailed)"
  SetOutPath "$InstallParent"
  IfErrors failed
  ReadRegStr $PreviousVersion HKCU "${UNINSTALL_KEY}" "${PENDING_VERSION}"
  ClearErrors
  ${If} $UpdateMode == "commit"
  ${OrIf} $UpdateMode == "rollback"
    Call FinishUpdate
    IfErrors failed
    Goto done
  ${EndIf}
  ${If} $PreviousVersion != ""
    Goto pending
  ${EndIf}
  IfFileExists "$UpdateBackup" pending
  ${If} $UpdateMode == "backup"
    !insertmacro CheckOwnedFolder "$INSTDIR" unmanaged
    ReadRegStr $PreviousVersion HKCU "${UNINSTALL_KEY}" "DisplayVersion"
    IfErrors failed
    StrCmp $PreviousVersion "" failed
  ${EndIf}
  GetTempFileName $StageDir "$InstallParent"
  IfErrors failed
  Delete "$StageDir"
  SetOutPath "$StageDir"
  IfErrors failed
  SetOverwrite try
  File /r "${PACKAGE_ROOT}\*"
  IfErrors failed
  WriteUninstaller "$StageDir\Uninstall.exe"
  IfErrors failed
  FileOpen $0 "$StageDir\${OWNERSHIP_FILE}" w
  IfErrors failed
  FileWrite $0 "${INSTALL_ID}$\r$\n"
  FileClose $0
  IfErrors failed
  SetOutPath "$InstallParent"
  IfErrors failed
  ; Recheck the path immediately before replacing a directory or registering it.
  Call CheckDestination
  IfErrors failed
  StrCpy $FailureMessage "$(InstallFailed)"

  ${If} $UpdateMode == "backup"
    !insertmacro CheckOwnedFolder "$INSTDIR" unmanaged
    StrCpy $BackupDir "$UpdateBackup"
    WriteRegStr HKCU "${UNINSTALL_KEY}" "${PENDING_VERSION}" "$PreviousVersion"
    IfErrors failed
    StrCpy $PendingStarted 1
  ${Else}
    IfFileExists "$INSTDIR" 0 install_staged
    GetTempFileName $BackupDir "$InstallParent"
    IfErrors failed
    Delete "$BackupDir"
  ${EndIf}
  Rename "$INSTDIR" "$BackupDir"
  IfErrors failed
  StrCpy $HadPrevious 1

install_staged:
  Rename "$StageDir" "$INSTDIR"
  IfErrors restore
  SetOutPath "$INSTDIR"
  IfErrors rollback
  ${If} $Relocating == 1
    Call GuardPreviousUninstaller
    IfErrors rollback
  ${EndIf}

  WriteRegStr HKCU "${UNINSTALL_KEY}" "DisplayName" "${PRODUCT}"
  WriteRegStr HKCU "${UNINSTALL_KEY}" "Publisher" "${PUBLISHER}"
  WriteRegStr HKCU "${UNINSTALL_KEY}" "URLInfoAbout" "${WEBSITE}"
  !insertmacro RegisterPath "$INSTDIR"
  WriteRegDWORD HKCU "${UNINSTALL_KEY}" "NoModify" 1
  WriteRegDWORD HKCU "${UNINSTALL_KEY}" "NoRepair" 1
  IfErrors rollback
  WriteRegStr HKCU "${UNINSTALL_KEY}" "DisplayVersion" "${VERSION}"
  IfErrors rollback

  ${If} $Relocating == 1
    Goto publish_shortcut
  ${EndIf}
  IfFileExists "${SHORTCUT}" installed
publish_shortcut:
  GetTempFileName $ShortcutStage "$SMPROGRAMS"
  IfErrors rollback
  Delete "$ShortcutStage"
  IfErrors rollback
  StrCpy $ShortcutStage "$ShortcutStage.lnk"
  CreateShortCut "$ShortcutStage" "$INSTDIR\EvpRemote.exe"
  IfErrors rollback
  System::Call 'kernel32::MoveFileExW(w "$ShortcutStage", w "${SHORTCUT}", i 9) i .r0'
  StrCmp $0 0 rollback
  StrCpy $ShortcutStage ""

installed:
  ${If} $HadPrevious == 1
  ${AndIf} $UpdateMode != "backup"
    Push $BackupDir
    Call RemoveTree
  ${EndIf}
  ClearErrors
  Goto done

unmanaged:
  StrCpy $FailureMessage "$(UnmanagedFolder)"
  Goto failed

pending:
  StrCpy $FailureMessage "$(PendingUpdate)"
  Goto failed

rollback:
  SetOutPath "$InstallParent"
  ${If} $ShortcutStage != ""
    Delete "$ShortcutStage"
  ${EndIf}
  ${If} $RegisteredDir == ""
    DeleteRegKey HKCU "${UNINSTALL_KEY}"
  ${Else}
    !insertmacro RegisterPath "$RegisteredDir"
    WriteRegStr HKCU "${UNINSTALL_KEY}" "DisplayVersion" "$RegisteredVersion"
  ${EndIf}
  Push $INSTDIR
  Call RemoveTree

restore:
  ${If} $HadPrevious == 1
    ClearErrors
    Rename "$BackupDir" "$INSTDIR"
    ${If} ${Errors}
      StrCpy $FailureMessage "$(RestoreFailed)"
    ${Else}
      StrCpy $HadPrevious 0
    ${EndIf}
  ${EndIf}

failed:
  ${If} $StageDir != ""
    SetOutPath "$InstallParent"
  ${EndIf}
  ${If} $PendingStarted == 1
  ${AndIf} $HadPrevious != 1
    DeleteRegValue HKCU "${UNINSTALL_KEY}" "${PENDING_VERSION}"
  ${EndIf}
  ${If} $StageDir != ""
    Push $StageDir
    Call RemoveTree
  ${EndIf}
  MessageBox MB_ICONSTOP "$FailureMessage" /SD IDOK
  SetErrorLevel 1
  Abort

done:
SectionEnd

Section "Uninstall"
  StrCmp $INSTDIR $RegisteredDir 0 failed
  Push "$INSTDIR"
  Call un.ValidatePath
  IfErrors failed
  !insertmacro DestinationParent
  ReadRegStr $PreviousVersion HKCU "${UNINSTALL_KEY}" "${PENDING_VERSION}"
  StrCmp $PreviousVersion "" 0 pending
  IfFileExists "$UpdateBackup" pending
  !insertmacro CheckOwnedFolder "$INSTDIR" failed
  ClearErrors
  SetOutPath "$InstallParent"
  GetTempFileName $BackupDir "$InstallParent"
  IfErrors failed
  Delete "$BackupDir"
  Rename "$INSTDIR" "$BackupDir"
  IfErrors failed
  Push $BackupDir
  Call un.RemoveTree
  Delete "${SHORTCUT}"
  DeleteRegKey HKCU "${UNINSTALL_KEY}"
  Goto done

pending:
  MessageBox MB_ICONSTOP "$(PendingUpdate)" /SD IDOK
  SetErrorLevel 1
  Abort

failed:
  MessageBox MB_ICONSTOP "$(UninstallFailed)" /SD IDOK
  SetErrorLevel 1
  Abort

done:
SectionEnd
