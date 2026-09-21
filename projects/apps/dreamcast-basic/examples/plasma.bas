' PLASMA - palette animation with PALETTE in SCREEN 13
SCREEN 13
KEY OFF
FOR Y = 0 TO 49
  FOR X = 0 TO 79
    V = SIN(X / 9) + SIN(Y / 6) + SIN((X + Y) / 11) + SIN(SQR(X * X + Y * Y) / 7)
    C = 32 + INT((V + 4) * 15.9) MOD 128
    LINE (X * 4, Y * 4)-(X * 4 + 3, Y * 4 + 3), C, BF
  NEXT X
NEXT Y
LOCATE 1, 1: PRINT "Press any key";
P = 0
DO
  P = P + 2
  FOR I = 0 TO 127
    A = (I + P) * 6.2832 / 128
    R = INT(31.5 + 31 * SIN(A))
    G = INT(31.5 + 31 * SIN(A + 2.094))
    B = INT(31.5 + 31 * SIN(A + 4.188))
    PALETTE 32 + I, R + 256 * G + 65536 * B
  NEXT I
  T = TIMER: WHILE TIMER - T < .03: WEND
LOOP WHILE INKEY$ = ""
