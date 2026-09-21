' SNAKE - steer with the arrow keys or the D-pad. Eat the food;
' avoid the walls and your own tail. Esc quits.
SCREEN 13
KEY OFF
RANDOMIZE TIMER
W = 38: H = 22: MAXLEN = 400
DIM G(W + 1, H + 1), SX(MAXLEN), SY(MAXLEN)
PLAY "MB"

NewGame:
CLS
LINE (6, 14)-(313, 193), 9, B
FOR X = 0 TO W + 1: FOR Y = 0 TO H + 1: G(X, Y) = 0: NEXT Y, X
FOR X = 0 TO W + 1: G(X, 0) = 1: G(X, H + 1) = 1: NEXT X
FOR Y = 0 TO H + 1: G(0, Y) = 1: G(W + 1, Y) = 1: NEXT Y
HEAD = 0: TAIL = 1: LENGTH = 4: SCORE = 0: DELAY = .1
DX = 1: DY = 0: NDX = 1: NDY = 0
FOR I = 1 TO LENGTH
  HEAD = HEAD + 1: SX(HEAD) = 10 + I: SY(HEAD) = 11: G(10 + I, 11) = 1
  LINE (SX(HEAD) * 8, 8 + SY(HEAD) * 8)-STEP(6, 6), 10, BF
NEXT I
GOSUB PlaceFood

DO
  LOCATE 1, 1: PRINT "SNAKE   SCORE"; SCORE;
  T = TIMER
  DO
    K$ = INKEY$
    IF LEN(K$) = 2 THEN
      C = ASC(RIGHT$(K$, 1))
      IF C = 72 AND DY = 0 THEN NDX = 0: NDY = -1
      IF C = 80 AND DY = 0 THEN NDX = 0: NDY = 1
      IF C = 75 AND DX = 0 THEN NDX = -1: NDY = 0
      IF C = 77 AND DX = 0 THEN NDX = 1: NDY = 0
    END IF
  LOOP WHILE TIMER - T < DELAY
  DX = NDX: DY = NDY
  HX = SX(HEAD) + DX: HY = SY(HEAD) + DY
  IF G(HX, HY) = 1 THEN EXIT DO
  ATE = G(HX, HY)
  LINE (SX(HEAD) * 8, 8 + SY(HEAD) * 8)-STEP(6, 6), 2, BF
  HEAD = HEAD MOD MAXLEN + 1: SX(HEAD) = HX: SY(HEAD) = HY: G(HX, HY) = 1
  LINE (HX * 8, 8 + HY * 8)-STEP(6, 6), 10, BF
  IF ATE = 2 AND LENGTH < MAXLEN - 2 THEN
    LENGTH = LENGTH + 1: SCORE = SCORE + 10
    IF DELAY > .04 THEN DELAY = DELAY - .003
    SOUND 880, .5: SOUND 1320, .5
    GOSUB PlaceFood
  ELSE
    IF ATE = 2 THEN SCORE = SCORE + 10: GOSUB PlaceFood
    G(SX(TAIL), SY(TAIL)) = 0
    LINE (SX(TAIL) * 8, 8 + SY(TAIL) * 8)-STEP(6, 6), 0, BF
    TAIL = TAIL MOD MAXLEN + 1
  END IF
LOOP

PLAY "MF T200 O2 L8 E C <A2"
LOCATE 12, 9: PRINT " GAME OVER - SCORE"; SCORE;
LOCATE 14, 7: PRINT " Any key plays again, Esc quits ";
A$ = INPUT$(1)
PLAY "MB"
GOTO NewGame

PlaceFood:
DO
  FX = 1 + INT(RND * W): FY = 1 + INT(RND * H)
LOOP WHILE G(FX, FY) <> 0
G(FX, FY) = 2
LINE (FX * 8, 8 + FY * 8)-STEP(6, 6), 12, BF
RETURN
