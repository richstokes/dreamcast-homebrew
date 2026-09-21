' BREAKOUT - Left/Right set the paddle moving, Down stops it; or use the
' D-pad or analogue stick. Clear the wall to reach the next level. Esc quits.
SCREEN 13
KEY OFF
RANDOMIZE TIMER
ROWS = 6: COLS = 10
DIM B(ROWS - 1, COLS - 1)
PLAY "MB"
SCORE = 0: LIVES = 3: LEVEL = 1

NewLevel:
CLS
LINE (8, 199)-(8, 12), 7: LINE -(311, 12), 7: LINE -(311, 199), 7
FOR R = 0 TO ROWS - 1
  FOR C = 0 TO COLS - 1
    B(R, C) = 1
    LINE (10 + C * 30, 30 + R * 10)-STEP(28, 7), 40 + R * 8, BF
  NEXT C
NEXT R
LEFT = ROWS * COLS
PX = 140: OLDPX = -1: PV = 0

Serve:
BX = PX + 20: BY = 170: DX = 1.5 - INT(RND * 2) * 3: DY = -(1.6 + LEVEL * .4)
DO
  LOCATE 1, 2: PRINT "SCORE"; SCORE; " LIVES"; LIVES; " LEVEL"; LEVEL;
  T = TIMER
  K$ = INKEY$
  IF LEN(K$) = 2 THEN
    C = ASC(RIGHT$(K$, 1))
    IF C = 75 THEN PV = -5
    IF C = 77 THEN PV = 5
    IF C = 80 THEN PV = 0
  END IF
  S = STICK(2) - 128
  IF ABS(S) > 60 THEN PV = 0: PX = PX + SGN(S) * 5
  S = STICK(0) - 128
  IF ABS(S) > 30 THEN PV = 0: PX = PX + S / 14
  PX = PX + PV
  IF PX < 10 THEN PX = 10: PV = 0
  IF PX > 270 THEN PX = 270: PV = 0
  IF PX <> OLDPX THEN
    LINE (9, 186)-(310, 190), 0, BF
    LINE (PX, 186)-STEP(40, 4), 15, BF
    OLDPX = PX
  END IF

  LINE (BX - 1, BY - 1)-STEP(2, 2), 0, BF
  BX = BX + DX: BY = BY + DY
  IF BX < 11 OR BX > 308 THEN DX = -DX: BX = BX + DX: SOUND 500, .3
  IF BY < 15 THEN DY = -DY: BY = BY + DY: SOUND 500, .3
  IF BY >= 30 AND BY < 30 + ROWS * 10 THEN
    R = INT((BY - 30) / 10): C = INT((BX - 10) / 30)
    IF C >= 0 AND C < COLS THEN
      IF B(R, C) = 1 THEN
        B(R, C) = 0: LEFT = LEFT - 1: SCORE = SCORE + (ROWS - R) * 5
        LINE (10 + C * 30, 30 + R * 10)-STEP(28, 7), 0, BF
        DY = -DY: SOUND 700 + (ROWS - R) * 100, .4
      END IF
    END IF
  END IF
  IF DY > 0 AND BY >= 184 AND BY <= 190 AND BX >= PX - 2 AND BX <= PX + 42 THEN
    DY = -DY: BY = 183
    DX = (BX - PX - 20) / 8
    SOUND 300, .4
  END IF
  LINE (BX - 1, BY - 1)-STEP(2, 2), 14, BF
  IF LEFT = 0 THEN EXIT DO
  IF BY > 196 THEN EXIT DO
  WHILE TIMER - T < .016: WEND
LOOP

IF LEFT = 0 THEN
  PLAY "MF T220 O3 L16 C E G >C"
  PLAY "MB"
  LEVEL = LEVEL + 1
  GOTO NewLevel
END IF
LINE (BX - 2, BY - 2)-STEP(4, 4), 0, BF
LIVES = LIVES - 1
SOUND 110, 5
IF LIVES > 0 THEN GOTO Serve
LOCATE 1, 2: PRINT "SCORE"; SCORE; " LIVES"; LIVES; " LEVEL"; LEVEL;
LOCATE 13, 8: PRINT " GAME OVER - SCORE"; SCORE; " ";
LOCATE 15, 5: PRINT " Any key plays again, Esc quits ";
A$ = INPUT$(1)
SCORE = 0: LIVES = 3: LEVEL = 1
GOTO NewLevel
