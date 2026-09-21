' BOUNCE - animation, sound and INKEY$
SCREEN 13
KEY OFF
N = 6
DIM X(N), Y(N), DX(N), DY(N), C(N)
RANDOMIZE TIMER
FOR I = 1 TO N
  X(I) = 20 + RND * 280: Y(I) = 30 + RND * 150
  DX(I) = 1 + RND * 3: DY(I) = 1 + RND * 3
  C(I) = 32 + I * 12
NEXT I
LINE (4, 12)-(315, 195), 15, B
LOCATE 1, 1: PRINT "BOUNCE - press any key to stop";
PLAY "MB"
DO
  FOR I = 1 TO N
    CIRCLE (X(I), Y(I)), 5, 0
    X(I) = X(I) + DX(I): Y(I) = Y(I) + DY(I)
    IF X(I) < 11 OR X(I) > 308 THEN DX(I) = -DX(I): X(I) = X(I) + DX(I): SOUND 300 + I * 80, .5
    IF Y(I) < 19 OR Y(I) > 188 THEN DY(I) = -DY(I): Y(I) = Y(I) + DY(I): SOUND 600 + I * 80, .5
    CIRCLE (X(I), Y(I)), 5, C(I)
  NEXT I
  T = TIMER: WHILE TIMER - T < .02: WEND
LOOP WHILE INKEY$ = ""
SCREEN 0
PRINT "Done."
