set host=http://192.168.1.131:10000
:loop1
curl %host%/stats
curl %host%^?op=insert^&key=aaa^&value=AAA
curl %host%^?key=aaa
goto:loop1
