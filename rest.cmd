@rem Compliance and stress tester for SeaStar/demos/db_demo.ccc
@rem Author: AlexanderMihail@gmail.com
@rem Incept date: 2024.04.08
@rem Modified on: 2024.04.11
@rem Version 1.

@echo off
set host=http://192.168.1.131:10000
curl %host%/config?trace_level=1111
curl %host%^?op=purge
set pass1=0
:loop1
@set /a "reminder=%pass1% %% 10"
@echo:
@echo ====== PASS(%pass1%,%reminder%) ==========================
set pass2=0
:loop2
curl -s %host%^?op=insert^&key=aaa^&value=text_aaa%%0a >NUL 2>&1
curl -s %host%^?key=aaa>NUL 2>&1
curl -s %host%^?op=insert^&key=bbb^&value=text_bbb%%0a>NUL 2>&1
curl -s %host%^?op=insert^&key=ccc^&value=text_ccc%%0a>NUL 2>&1
curl -s %host%^?key=aaa>NUL 2>&1
curl -s %host%^?key=bbb>NUL 2>&1
curl -s %host%^?op=update^&key=aaa^&value=text_aaaa%%0a>NUL 2>&1
curl -s %host%^?op=update^&key=bbb^&value=text_bbbb%%0a>NUL 2>&1
curl -s %host%^?op=update^&key=ccc^&value=text_cccc%%0a>NUL 2>&1
curl -s %host%^?op=update^&key=aaa^&value=text_aa%%0a>NUL 2>&1
curl -s %host%^?op=update^&key=bbb^&value=text_bb%%0a>NUL 2>&1
@set /a "pass2=%pass2%+1"
if %pass1% GTR 0 IF %pass2% LSS 10 goto :loop2
if %reminder% EQU 0 (
	curl %host%^?op=compact>NUL 2>&1
)
curl -s %host%^?key=aaa>NUL 2>&1
curl -s %host%^?key=bbb>NUL 2>&1
curl -s %host%^?key=ccc>NUL 2>&1
curl -s %host%^?key=ddd>NUL 2>&1
curl -s %host%^?op=delete^&key=bbb>NUL 2>&1
curl -s %host%^?op=list^&key=ALL>NUL 2>&1
curl -s %host%/stats
curl %host%/config?trace_level=0000>NUL 2>&1
if %pass1% EQU 100 (
	curl %host%/quit
)
@set /a "pass1=%pass1%+1"
goto :loop1
