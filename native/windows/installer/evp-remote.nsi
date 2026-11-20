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
!define INSTALL_PARENT "$LOCALAPPDATA\Programs"
!define INSTALL_PATH "${INSTALL_PARENT}\${INSTALL_NAME}"
!define UPDATE_BACKUP "${INSTALL_PARENT}\.${INSTALL_ID}-update-backup"
!define PENDING_VERSION "PendingVersion"
!define OWNERSHIP_FILE ".evp-remote-install"
!define SHORTCUT "$SMPROGRAMS\${INSTALL_NAME}.lnk"

Name "${PRODUCT}"
OutFile "${OUTPUT_EXE}"
InstallDir "${INSTALL_PATH}"
VIProductVersion "${NUMERIC_VERSION}"
VIFileVersion "${NUMERIC_VERSION}"

!define MUI_ICON "${APP_ICON}"
!define MUI_UNICON "${APP_ICON}"
!define MUI_ABORTWARNING
!define MUI_FINISHPAGE_RUN "$INSTDIR\EvpRemote.exe"
!define MUI_FINISHPAGE_RUN_NOTCHECKED
!define MUI_FINISHPAGE_NOREBOOTSUPPORT
!insertmacro MUI_PAGE_WELCOME
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
  VIAddVersionKey /LANG=${LANGUAGE} "LegalCopyright" "${PUBLISHER}"
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

Var StageDir
Var BackupDir
Var HadPrevious
Var CreatedShortcut
Var FailureMessage
Var UpdateMode
Var PreviousVersion
Var PendingStarted
Var DiscardDir

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
  ; The updater and Windows shortcuts share a stable per-user path.
  StrCpy $INSTDIR "${INSTALL_PATH}"
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
!insertmacro RemoveTree ""
!insertmacro RemoveTree "un."

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

; Commit and rollback use the same guarded paths as ordinary setup. The backup
; exists until the update helper confirms that the replacement started.
Function FinishUpdate
  StrCpy $BackupDir "${UPDATE_BACKUP}"
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

  GetTempFileName $DiscardDir "${INSTALL_PARENT}"
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
  StrCpy $FailureMessage "$(InstallFailed)"
  ClearErrors
  SetOutPath "${INSTALL_PARENT}"
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
  IfFileExists "${UPDATE_BACKUP}" pending
  ${If} $UpdateMode == "backup"
    !insertmacro CheckOwnedFolder "$INSTDIR" unmanaged
    ReadRegStr $PreviousVersion HKCU "${UNINSTALL_KEY}" "DisplayVersion"
    IfErrors failed
    StrCmp $PreviousVersion "" failed
  ${EndIf}
  IfFileExists "$INSTDIR" 0 staging
  !insertmacro CheckOwnedFolder "$INSTDIR" unmanaged

staging:
  GetTempFileName $StageDir "${INSTALL_PARENT}"
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
  SetOutPath "${INSTALL_PARENT}"
  IfErrors failed

  ${If} $UpdateMode == "backup"
    !insertmacro CheckOwnedFolder "$INSTDIR" unmanaged
    StrCpy $BackupDir "${UPDATE_BACKUP}"
    WriteRegStr HKCU "${UNINSTALL_KEY}" "${PENDING_VERSION}" "$PreviousVersion"
    IfErrors failed
    StrCpy $PendingStarted 1
  ${Else}
    IfFileExists "$INSTDIR" 0 install_staged
    !insertmacro CheckOwnedFolder "$INSTDIR" unmanaged
    GetTempFileName $BackupDir "${INSTALL_PARENT}"
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
  IfFileExists "${SHORTCUT}" registered
  CreateShortCut "${SHORTCUT}" "$INSTDIR\EvpRemote.exe"
  IfErrors rollback
  StrCpy $CreatedShortcut 1

registered:
  WriteRegStr HKCU "${UNINSTALL_KEY}" "DisplayName" "${PRODUCT}"
  WriteRegStr HKCU "${UNINSTALL_KEY}" "Publisher" "${PUBLISHER}"
  WriteRegStr HKCU "${UNINSTALL_KEY}" "URLInfoAbout" "${WEBSITE}"
  WriteRegStr HKCU "${UNINSTALL_KEY}" "DisplayIcon" "$INSTDIR\EvpRemote.exe,0"
  WriteRegStr HKCU "${UNINSTALL_KEY}" "InstallLocation" "$INSTDIR"
  WriteRegStr HKCU "${UNINSTALL_KEY}" "UninstallString" '$\"$INSTDIR\Uninstall.exe$\"'
  WriteRegStr HKCU "${UNINSTALL_KEY}" "QuietUninstallString" '$\"$INSTDIR\Uninstall.exe$\" /S'
  WriteRegDWORD HKCU "${UNINSTALL_KEY}" "NoModify" 1
  WriteRegDWORD HKCU "${UNINSTALL_KEY}" "NoRepair" 1
  IfErrors rollback
  WriteRegStr HKCU "${UNINSTALL_KEY}" "DisplayVersion" "${VERSION}"
  IfErrors rollback

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
  SetOutPath "${INSTALL_PARENT}"
  ${If} $CreatedShortcut == 1
    Delete "${SHORTCUT}"
  ${EndIf}
  ${If} $HadPrevious != 1
    DeleteRegKey HKCU "${UNINSTALL_KEY}"
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
  SetOutPath "${INSTALL_PARENT}"
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
  StrCmp $INSTDIR "${INSTALL_PATH}" 0 failed
  ReadRegStr $PreviousVersion HKCU "${UNINSTALL_KEY}" "${PENDING_VERSION}"
  StrCmp $PreviousVersion "" 0 pending
  IfFileExists "${UPDATE_BACKUP}" pending
  !insertmacro CheckOwnedFolder "$INSTDIR" failed
  ClearErrors
  SetOutPath "${INSTALL_PARENT}"
  GetTempFileName $BackupDir "${INSTALL_PARENT}"
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
