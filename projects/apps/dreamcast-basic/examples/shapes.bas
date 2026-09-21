' SHAPES - a tour of the graphics statements
SCREEN 12
CLS
LOCATE 1, 24: COLOR 14: PRINT "DREAMCAST BASIC GRAPHICS"
' Starfield
FOR I = 1 TO 150
  PSET (RND * 640, 40 + RND * 250), 7 + INT(RND * 2) * 8
NEXT I
' Sun: filled circle
CIRCLE (520, 110), 50, 14
PAINT (520, 110), 14, 14
' Ground, then hills: arcs closed by the ground and flood filled
LINE (0, 400)-(639, 479), 2, BF
CIRCLE (150, 400), 140, 2, 0, 3.14159
PAINT (150, 330), 2, 2
CIRCLE (500, 400), 110, 2, 0, 3.14159
PAINT (500, 350), 2, 2
' House from boxes
LINE (250, 300)-(390, 420), 6, BF
LINE (250, 300)-(390, 420), 15, B
LINE (300, 350)-(340, 420), 4, BF
LINE (265, 320)-(290, 345), 11, BF
LINE (350, 320)-(375, 345), 11, BF
' Roof with DRAW
DRAW "BM240,300 C12 E80 F80 L160"
PAINT (320, 280), 12, 12
' Colour bars
FOR C = 0 TO 15
  LINE (40 + C * 35, 60)-(70 + C * 35, 80), C, BF
  LINE (40 + C * 35, 60)-(70 + C * 35, 80), 15, B
NEXT C
' Spirograph
FOR A = 0 TO 6.283 STEP .05
  X = 110 + COS(A) * 60 + COS(A * 7) * 20
  Y = 170 + SIN(A) * 60 + SIN(A * 7) * 20
  IF A = 0 THEN PSET (X, Y), 13 ELSE LINE -(X, Y), 13
NEXT A
LOCATE 27, 20: COLOR 15: PRINT "PSET LINE CIRCLE PAINT DRAW";
PLAY "T180 O3 L8 C E G >C< G E C2"
