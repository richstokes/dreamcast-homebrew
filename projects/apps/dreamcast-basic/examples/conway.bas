' CONWAY - John Conway's Game of Life on a 60 x 36 grid.
' R reseeds at random, G places a Gosper glider gun, Space pauses, Esc quits.
SCREEN 13
KEY OFF
RANDOMIZE TIMER
W = 60: H = 36
DIM A(W + 1, H + 1), N(W + 1, H + 1)
GOSUB Soup

DO
  LOCATE 1, 1: PRINT "LIFE  GEN"; GEN; " CELLS"; POP; "  ";
  ' Every live cell adds one to each of its eight neighbours
  FOR Y = 1 TO H
    FOR X = 1 TO W
      IF A(X, Y) THEN
        N(X - 1, Y - 1) = N(X - 1, Y - 1) + 1: N(X, Y - 1) = N(X, Y - 1) + 1
        N(X + 1, Y - 1) = N(X + 1, Y - 1) + 1: N(X - 1, Y) = N(X - 1, Y) + 1
        N(X + 1, Y) = N(X + 1, Y) + 1: N(X - 1, Y + 1) = N(X - 1, Y + 1) + 1
        N(X, Y + 1) = N(X, Y + 1) + 1: N(X + 1, Y + 1) = N(X + 1, Y + 1) + 1
      END IF
    NEXT X
  NEXT Y
  ' Birth on three neighbours, survival on two or three
  POP = 0
  FOR Y = 1 TO H
    FOR X = 1 TO W
      C = N(X, Y): N(X, Y) = 0
      IF A(X, Y) THEN
        IF C = 2 OR C = 3 THEN POP = POP + 1 ELSE A(X, Y) = 0: LINE (X * 5 + 5, Y * 5 + 8)-STEP(3, 3), 0, BF
      ELSE
        IF C = 3 THEN A(X, Y) = 1: POP = POP + 1: LINE (X * 5 + 5, Y * 5 + 8)-STEP(3, 3), 50, BF
      END IF
    NEXT X
  NEXT Y
  GEN = GEN + 1
  K$ = UCASE$(INKEY$)
  IF K$ = " " THEN LOCATE 1, 30: PRINT "PAUSED";: K$ = UCASE$(INPUT$(1)): LOCATE 1, 30: PRINT "      ";
  IF K$ = "R" THEN GOSUB Soup
  IF K$ = "G" THEN GOSUB Gun
LOOP UNTIL K$ = CHR$(27)
END

Wipe:
CLS
LINE (8, 11)-(311, 193), 8, B
FOR Y = 0 TO H + 1: FOR X = 0 TO W + 1: A(X, Y) = 0: N(X, Y) = 0: NEXT X, Y
GEN = 0: POP = 0
RETURN

Soup:
GOSUB Wipe
FOR Y = 1 TO H
  FOR X = 1 TO W
    IF RND < .28 THEN A(X, Y) = 1: POP = POP + 1: LINE (X * 5 + 5, Y * 5 + 8)-STEP(3, 3), 50, BF
  NEXT X
NEXT Y
RETURN

Gun:
GOSUB Wipe
RESTORE GunData
FOR I = 1 TO 36
  READ X, Y
  A(X + 2, Y + 2) = 1: POP = POP + 1
  LINE ((X + 2) * 5 + 5, (Y + 2) * 5 + 8)-STEP(3, 3), 50, BF
NEXT I
RETURN

GunData:
DATA 1,5, 1,6, 2,5, 2,6, 11,5, 11,6, 11,7, 12,4, 12,8, 13,3, 13,9, 14,3, 14,9
DATA 15,6, 16,4, 16,8, 17,5, 17,6, 17,7, 18,6, 21,3, 21,4, 21,5, 22,3, 22,4
DATA 22,5, 23,2, 23,6, 25,1, 25,2, 25,6, 25,7, 35,3, 35,4, 36,3, 36,4
