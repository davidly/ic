@echo off
del ic.obj
del ic.exe
del ic.pdb
@echo on

cl /nologo ic.cxx /Ox /Qpar /O2 /Oi /Ob2 /EHac /Zi /Gy /DNDEBUG /DUNICODE /D_AMD64_ /link ntdll.lib /OPT:REF
REM cl /nologo ic.cxx /Ox /Qpar /O2 /Oi /Ob2 /EHac /Zi /Gy /DUNICODE /D_AMD64_ /link ntdll.lib /OPT:REF


