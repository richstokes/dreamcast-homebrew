# Ridgeline's truck physics built for the host (see sim.c).
GAME = ../..
CFLAGS = -std=gnu17 -O1 -g -Wall -DRL_AI_TRACE -Wno-unused-function -Istub -I$(GAME)/src -I$(GAME)/assets/generated -I$(GAME)/../../common \
	-DLEVEL_BIN_FILE='"$(abspath $(GAME)/assets/generated/level.bin)"'
SRC = sim.c stubs.c level_bin.c $(GAME)/src/truck.c $(GAME)/src/terrain.c $(GAME)/src/ai.c

sim: $(SRC) $(wildcard $(GAME)/src/*.h) $(wildcard $(GAME)/assets/generated/*.h) $(GAME)/assets/generated/level.bin
	cc $(CFLAGS) -o $@ $(SRC) -lm

clean:
	rm -f sim
