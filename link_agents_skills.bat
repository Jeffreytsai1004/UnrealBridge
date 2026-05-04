@echo off
setlocal

rem Expose .claude/skills as the cross-tool .agents/skills via an NTFS junction.
rem
rem Several agent runtimes (Gemini CLI, OpenCode, Cursor, ...) follow the
rem "Agent Skills open standard" and read .agents/skills/<name>/SKILL.md
rem when present. Rather than copy the skill tree per tool (the way
rem convert_claude_skills_to_codex.bat does for Codex), this script
rem creates a directory junction so they all share the same source of truth.
rem
rem Junctions:
rem   - work without admin / Developer Mode
rem   - are NTFS-only (Windows; same-volume only)
rem   - are .gitignored so they don't end up in commits as duplicate content
rem
rem Re-runnable: existing junction is removed first, then re-created.

set "SRC=%~dp0.claude\skills"
set "DST=%~dp0.agents\skills"

if not exist "%SRC%" (
    echo ERROR: %SRC% does not exist. Nothing to link.
    exit /b 1
)

rem Make sure parent .agents\ exists
if not exist "%~dp0.agents" mkdir "%~dp0.agents"

rem Drop any prior junction or directory at DST (idempotent re-run)
if exist "%DST%" (
    rmdir "%DST%" 2>nul
    if exist "%DST%" (
        echo WARNING: %DST% already exists and could not be removed.
        echo Remove it manually and re-run.
        exit /b 1
    )
)

mklink /J "%DST%" "%SRC%" >nul
if errorlevel 1 (
    echo ERROR: mklink failed. NTFS junctions require Windows + same volume.
    exit /b 1
)

echo Junction created:
echo   %DST%
echo     --^> %SRC%
echo.
echo Gemini CLI / OpenCode / Cursor that read .agents/skills will now see
echo the same content as .claude/skills (single source of truth).

endlocal
