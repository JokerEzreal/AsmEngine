@echo off
echo === One-Click Fix for AsmEngine ===
echo.

:: Create compatibility fixes inline
echo Creating compatibility layer...

:: Fix 1: Create C fix for _snprintf
echo #include ^<stdio.h^> > compat_fix.c
echo #include ^<stdarg.h^> >> compat_fix.c
echo int _snprintf(char* buf, size_t cnt, const char* fmt, ...) { >> compat_fix.c
echo     va_list args; >> compat_fix.c
echo     va_start(args, fmt); >> compat_fix.c
echo     int ret = vsnprintf(buf, cnt, fmt, args); >> compat_fix.c
echo     va_end(args); >> compat_fix.c
echo     return ret; >> compat_fix.c
echo } >> compat_fix.c

:: Compile everything together
echo.
echo Compiling with compatibility fixes...
cl main.cpp compat_fix.c ^
   /MT /EHsc /std:c++17 /O2 ^
   /I"..\AsmEngine\include" ^
   /D_CRT_SECURE_NO_WARNINGS ^
   /link ^
   /LIBPATH:"..\AsmEngine\x64\Release" ^
   /LIBPATH:"." ^
   AsmEngine.lib ^
   keystone.lib ^
   kernel32.lib user32.lib advapi32.lib ntdll.lib psapi.lib ^
   legacy_stdio_definitions.lib ^
   /FORCE:MULTIPLE ^
   /NODEFAULTLIB:MSVCRT ^
   /OUT:GTA5Trainer.exe

:: Clean up
del compat_fix.c *.obj 2>nul

:: Check result
echo.
if exist GTA5Trainer.exe (
    color 0A
    echo ========================================
    echo    SUCCESS! GTA5Trainer.exe created!
    echo ========================================
    echo.
    echo Next steps:
    echo 1. Copy keystone.dll to this directory
    echo 2. Run GTA5Trainer.exe as Administrator
) else if exist main.exe (
    move main.exe GTA5Trainer.exe
    color 0A
    echo ========================================
    echo    SUCCESS! GTA5Trainer.exe created!
    echo ========================================
) else (
    color 0C
    echo ========================================
    echo    FAILED! See errors above
    echo ========================================
    echo.
    echo Try manual command:
    echo cl main.cpp /MT /I"..\AsmEngine\include" /link /LIBPATH:"..\AsmEngine\x64\Release" AsmEngine.lib keystone.lib /FORCE
)

pause