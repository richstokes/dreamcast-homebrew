# Ridgeline's truck physics built for the host (see sim.c).
GAME = ../..
GEN = $(GAME)/assets/generated
CFLAGS = -std=gnu17 -O1 -g -Wall -DRL_AI_TRACE -Wno-unused-function -Istub -I$(GAME)/src -I$(GEN) -I$(GAME)/../../common \
	-DLEVEL_DIR='"$(abspath $(GEN))"'
SRC = sim.c stubs.c level_bin.c $(GAME)/src/truck.c $(GAME)/src/terrain.c $(GAME)/src/level.c $(GAME)/src/ai.c $(GAME)/src/recover.c

sim: $(SRC) $(wildcard $(GAME)/src/*.h) $(wildcard $(GEN)/*.h)
	cc $(CFLAGS) -o $@ $(SRC) -lm

clean:
	rm -f sim
