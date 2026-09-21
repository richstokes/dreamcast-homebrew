10 REM The famous one-liner, given a finish line so it ends
20 RANDOMIZE TIMER
30 FOR I = 1 TO 76 * 24
40 PRINT CHR$(47 + 45 * INT(RND(1) * 2));
50 NEXT I
