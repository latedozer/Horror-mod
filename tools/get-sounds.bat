@echo off
rem Windows version of get-sounds.sh. Pulls the thunder and ambient sounds the plugin plays out of
rem YOUR OWN copy of Diablo II: Resurrected and converts them to WAV. The game's sound files are
rem Blizzard's and are not part of this repository or the release.
rem
rem Needs, in this folder or on PATH:
rem   cx.exe      - the CASC extractor (in the release's sound-tools zip, or build casc_extract\cx.c)
rem   ffmpeg.exe  - https://ffmpeg.org/download.html (any recent Windows build)
rem and CascLib.dll, which ships with D2RMM in its tools folder.
rem
rem   get-sounds.bat "C:\Program Files (x86)\Diablo II Resurrected" "C:\D2RMM\tools\CascLib.dll" [output folder]
rem
rem Writes a horror-night-sounds folder (default: next to this file). Copy it to
rem <game>\mods\<mod>\d2rloader\horror-night-sounds\ - the plugin reads the sounds from there.
setlocal EnableDelayedExpansion

set "GAME=%~1"
set "CASCLIB=%~f2"
set "OUT=%~3"
if "%GAME%"=="" goto usage
if not exist "%GAME%\" (echo Game folder not found: !GAME! & goto usage)
if not exist "%CASCLIB%" (echo CascLib.dll not found: !CASCLIB! & goto usage)
if "%OUT%"=="" set "OUT=%~dp0horror-night-sounds"
for %%I in ("%OUT%") do set "OUT=%%~fI"

set "HERE=%~dp0"
set "CX=%HERE%cx.exe"
if not exist "%CX%" (
	for %%I in (cx.exe) do set "CX=%%~$PATH:I"
)
if "%CX%"=="" (echo cx.exe not found - put it next to this file & exit /b 1)
set "FFMPEG=%HERE%ffmpeg.exe"
if not exist "%FFMPEG%" (
	for %%I in (ffmpeg.exe) do set "FFMPEG=%%~$PATH:I"
)
if "%FFMPEG%"=="" (echo ffmpeg.exe not found - put it next to this file or on PATH & exit /b 1)

set "WORK=%TEMP%\horror-night-sounds-%RANDOM%%RANDOM%"
mkdir "%WORK%\out" || exit /b 1
set "NAMES=%WORK%\names.txt"
set "AMB=breath1 breath3 breath6 breath8 moan1 moan2 moan3 moan4 moan5 moan6 moan7 moan8 scream1 scream3 scream5 scream7 raven1 raven3"

(
	for %%N in (1 2 3 4) do echo data:data\hd\global\sfx\ambient\event\thunder_norm_%%N_hd.flac
	for %%N in (%AMB%) do echo data:data\global\sfx\ambient\event\%%N.flac
	echo data:data\global\sfx\monster\diablo\laugh1.flac
) > "%NAMES%"

echo Reading the sounds from the game...
"%CX%" "%CASCLIB%" "%GAME%" get "%WORK%\out" < "%NAMES%"
if errorlevel 1 (echo Could not read the game's data. & goto fail)

mkdir "%OUT%\amb" 2>nul
set "SRC=%WORK%\out\data"
echo Converting...
for %%N in (1 2 3 4) do (
	"%FFMPEG%" -v error -y -i "%SRC%\hd\global\sfx\ambient\event\thunder_norm_%%N_hd.flac" -ac 1 -ar 32000 -af "bass=g=4:f=90,volume=12dB,alimiter=limit=0.95:level=false:attack=3:release=120" -c:a pcm_s16le -bitexact "%OUT%\thunder%%N.wav"
	if errorlevel 1 (echo Failed: thunder %%N & goto fail)
)
set /a I=1
for %%N in (%AMB%) do (
	"%FFMPEG%" -v error -y -i "%SRC%\global\sfx\ambient\event\%%N.flac" -ac 1 -ar 22050 -t 9 -af "volume=3dB,alimiter=limit=0.95:level=false" -c:a pcm_s16le -bitexact "%OUT%\amb\a!I!.wav"
	if errorlevel 1 (echo Failed: %%N & goto fail)
	set /a I+=1
)
"%FFMPEG%" -v error -y -i "%SRC%\global\sfx\monster\diablo\laugh1.flac" -ac 1 -ar 22050 -t 9 -af "volume=3dB,alimiter=limit=0.95:level=false" -c:a pcm_s16le -bitexact "%OUT%\amb\a!I!.wav"
if errorlevel 1 (echo Failed: laugh1 & goto fail)

rmdir /s /q "%WORK%"
echo.
echo Sounds written to %OUT%
echo Copy that folder to ^<game^>\mods\^<mod^>\d2rloader\horror-night-sounds\
exit /b 0

:fail
rmdir /s /q "%WORK%" 2>nul
exit /b 1

:usage
echo usage: %~nx0 "<game folder>" "<CascLib.dll>" [output folder]
exit /b 1
