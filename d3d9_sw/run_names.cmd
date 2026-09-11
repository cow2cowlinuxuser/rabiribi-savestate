@echo off
set CDB=C:\Program Files (x86)\Windows Kits\10\Debuggers\x64\cdb.exe
"%CDB%" -cf names.cdb C:\Windows\System32\cmd.exe /c exit < NUL > names.txt 2>&1
exit /b %ERRORLEVEL%
