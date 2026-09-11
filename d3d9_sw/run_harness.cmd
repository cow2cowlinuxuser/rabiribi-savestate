@echo off
rem Drives the harness under cdb. A batch file rather than an inline command
rem because the debugger path contains both spaces and parentheses, and stdin is
rem redirected from NUL so an unexpected prompt ends the run instead of hanging
rem it - which is how the first attempt was lost.
set CDB=C:\Program Files (x86)\Windows Kits\10\Debuggers\x64\cdb.exe
"%CDB%" -cf harness.cdb ss_harness.exe %1 %2 %3 < NUL > harness_out.txt 2>&1
exit /b %ERRORLEVEL%
