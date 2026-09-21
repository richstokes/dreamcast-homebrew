' MANDEL - the Mandelbrot set in 256 colours
SCREEN 13
KEY OFF
MAXI = 28
FOR PY = 0 TO 99
  CI = -1.15 + PY * 2.3 / 100
  FOR PX = 0 TO 159
    CR = -2.15 + PX * 3.1 / 160
    ZR = 0: ZI = 0: I = 0
    DO
      T = ZR * ZR - ZI * ZI + CR
      ZI = 2 * ZR * ZI + CI
      ZR = T
      I = I + 1
    LOOP WHILE I < MAXI AND ZR * ZR + ZI * ZI < 4
    IF I = MAXI THEN C = 0 ELSE C = 32 + I * 3
    LINE (PX * 2, PY * 2)-(PX * 2 + 1, PY * 2 + 1), C, BF
  NEXT PX
  IF INKEY$ <> "" THEN END
NEXT PY
LOCATE 25, 1: PRINT "Press any key";
A$ = INPUT$(1)
