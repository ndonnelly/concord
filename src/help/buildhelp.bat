@echo off
rem echo ===== Building snap help file =====
rem perl build_helpproject.pl -b -c ..\..\..\ms\build\debug -c ..\..\ms\build\debug snaphelp help -x gridfile_c
echo ===== Building concord help file =====
perl build_helpproject.pl -b concord help/programs/concord help/coordsys -x linzdef trigfile gridfile_s
