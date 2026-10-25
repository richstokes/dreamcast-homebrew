package lobby

import (
	"bytes"
	"compress/gzip"
	"encoding/binary"
	"testing"
	"time"

	"github.com/richstokes/dreamcast-homebrew/projects/games/dreamkarts/server/demo"
)

func TestSpectators(t *testing.T) {
	h := newHarness(t)
	var players []*client
	for i := 0; i < MaxPlayers; i++ {
		c := h.client(uint16(10+i), uint32(100+i))
		c.hello("p")
		c.send(TypeJoin, []byte{2})
		players = append(players, c)
	}
	// A seventh console can't play, but can watch.
	late := h.client(30, 300)
	late.hello("late")
	late.send(TypeJoin, []byte{2})
	if r := late.last(TypeReject); r == nil || r[8] != RejectLobbyFul {
		t.Fatalf("want lobby full, got %v", r)
	}
	w := h.client(31, 301)
	w.hello("watcher")
	w.send(TypeJoin, []byte{2, JoinSpectate})
	v := w.lobby()
	if v.id != 2 || v.you != SpectatorSlot || len(v.players) != MaxPlayers {
		t.Fatalf("spectator view %+v", v)
	}
	b := w.last(TypeLobby)
	if tail := b[22+MaxPlayers*PlayerLen:]; len(tail) != 7 || tail[0] != 1 {
		t.Fatalf("watcher count tail %v", tail)
	}
	// Its wishes change nothing.
	w.send(TypeWish, []byte{0, 4, 10, 2, 9})
	if h.s.Lobbies[2].Track == 4 || h.s.Lobbies[2].State != StateWaiting {
		t.Fatalf("a spectator changed the lobby")
	}
	// It is sent the race and is not a racer.
	lead := players[0]
	lead.wish(0, 1, 3, 1)
	lead.nonce++
	lead.wish(0, 1, 3, 1)
	h.advance(CountdownLen + time.Second)
	for _, c := range players {
		c.kart(h.s.Lobbies[2].RaceID, 0, 1, 100, 1000)
	}
	h.advance(200 * time.Millisecond)
	r := w.last(TypeRace)
	if r == nil || r[9] != MaxPlayers {
		t.Fatalf("race packet %v", r)
	}
	if len(h.s.racers(h.s.Lobbies[2])) != MaxPlayers {
		t.Fatal("the spectator became a racer")
	}
	// The lobby list counts watchers after the entries.
	w.send(TypeList, nil)
	l := w.last(TypeLobbies)
	if tail := l[9+MaxLobbies*14:]; len(tail) != MaxLobbies*2 || tail[2*2] != 1 {
		t.Fatalf("lobbies tail %v", tail)
	}
	// Leaving, and the lobby emptying, keep the books straight.
	w.send(TypeLeave, nil)
	if len(h.s.Lobbies[2].Watchers) != 0 {
		t.Fatal("watcher not removed")
	}
	w.send(TypeJoin, []byte{5, JoinSpectate})
	w.send(TypeJoin, []byte{5, JoinSpectate})
	if len(h.s.Lobbies[5].Watchers) != 1 {
		t.Fatalf("watchers %d", len(h.s.Lobbies[5].Watchers))
	}
	// A watcher can step into a free seat.
	w.send(TypeJoin, []byte{5})
	if len(h.s.Lobbies[5].Watchers) != 0 || h.s.Lobbies[5].count() != 1 || w.lobby().you != 0 {
		t.Fatalf("watcher did not take a seat")
	}
	// Silent watchers time out.
	x := h.client(32, 302)
	x.hello("x")
	x.send(TypeJoin, []byte{7, JoinSpectate})
	h.advance(PlayerTimeout + time.Second)
	if len(h.s.Lobbies[7].Watchers) != 0 {
		t.Fatal("silent watcher kept")
	}
}

// fakeRace records six karts that cross the line one by one after secs,
// made for lobby 9 as race raceID. Kart 0 holds a spear from the first
// second and throws it at kart 1 a second later, where it flies until the
// last second.
func fakeRace(t *testing.T, track byte, secs float64, raceID byte) *demo.Race {
	frames := int(secs*demo.Rate) + 3*demo.Rate
	var b bytes.Buffer
	b.WriteString("DKR3")
	b.Write([]byte{track, 3, 2, MaxPlayers})
	binary.Write(&b, binary.LittleEndian, uint32(frames))
	b.Write([]byte{9, raceID, 0, 0})
	for f := 0; f < frames; f++ {
		for k := 0; k < MaxPlayers; k++ {
			e := make([]byte, demo.KartLen)
			ms := uint32(f * 1000 / demo.Rate)
			fin := uint32((secs + float64(k)*0.25) * 1000)
			e[0] = 2
			e[13] = 1
			if ms >= fin {
				e[0] |= KartFinished
				e[13] = 4
				ms = fin
			}
			binary.LittleEndian.PutUint16(e[15:], uint16(f*16))
			binary.LittleEndian.PutUint16(e[9:], uint16(30*64)) // rocket starts: moving from the green light
			e[0] |= KartBoosting
			binary.LittleEndian.PutUint32(e[17:], ms)
			e[demo.KartTarget] = demo.NoTarget
			if k == 0 && f >= demo.Rate && f < 2*demo.Rate {
				e[demo.KartItems] = 2 // holding a Chaos Spear
			}
			if k == 0 && f >= 2*demo.Rate && f < frames-demo.Rate {
				e[demo.KartSeqs] = 1 // spear 1 in flight
				e[demo.KartTarget] = 1
			}
			b.Write(e)
		}
	}
	var z bytes.Buffer
	zw := gzip.NewWriter(&z)
	zw.Write(b.Bytes())
	zw.Close()
	r, err := demo.Parse(z.Bytes())
	if err != nil {
		t.Fatal(err)
	}
	return r
}

func TestDemoLobby(t *testing.T) {
	h := newHarness(t)
	races := []*demo.Race{fakeRace(t, 0, 5, 1), fakeRace(t, 2, 6, 3), fakeRace(t, 4, 5, 5)}
	h.s.EnableDemo(9, races, 1, h.now)
	d := h.s.demo
	l := h.s.Lobbies[9]
	if l.count() != MaxPlayers || !l.Demo {
		t.Fatalf("demo lobby has %d bots", l.count())
	}
	c := h.client(1, 11)
	c.hello("fan")
	c.send(TypeJoin, []byte{9})
	if r := c.last(TypeReject); r == nil || r[8] != RejectLobbyFul {
		t.Fatal("a person joined the demo lobby")
	}
	c.send(TypeJoin, []byte{9, JoinSpectate})
	if v := c.lobby(); v.you != SpectatorSlot {
		t.Fatalf("not watching: %+v", v)
	}
	// Watchers stay through it all, sending only pings.
	adv := func(d time.Duration) {
		for ; d > 0; d -= time.Second {
			step := time.Second
			if d < step {
				step = d
			}
			h.advance(step)
			c.send(TypePing, nil)
		}
	}
	// until advances (pinging) until the lobby reaches state, within max.
	until := func(race int, state byte, max time.Duration) {
		for waited := time.Duration(0); l.State != state; waited += time.Second {
			if waited > max {
				t.Fatalf("race %d: state %d, want %d", race, l.State, state)
			}
			adv(time.Second)
		}
	}
	var tracks []byte
	for race := 0; race < 4; race++ {
		begin := h.now
		until(race, StateCountdown, DemoWait+2*time.Second)
		if waited := h.now.Sub(begin); waited < DemoWait-time.Second {
			t.Fatalf("race %d: started after %v in the lobby", race, waited)
		}
		tracks = append(tracks, l.Track)
		// On the grid the bots hold still: no speed, no boost, or consoles
		// would run them on over the line.
		adv(time.Second)
		for i, r := 0, c.last(TypeRace); r != nil && i < int(r[9]); i++ {
			e := r[10+(3+demo.KartLen)*i+3:]
			if binary.LittleEndian.Uint16(e[9:]) != 0 || e[0]&KartBoosting != 0 {
				t.Fatalf("race %d: slot %d moving on the grid: speed %d flags %#x", race, r[10+(3+demo.KartLen)*i],
					binary.LittleEndian.Uint16(e[9:]), e[0])
			}
		}
		if l.RaceID != d.race.RaceID {
			t.Fatalf("race %d: id %d, recording %s wants %d", race, l.RaceID, d.race.Name, d.race.RaceID)
		}
		// Mid-race the bot on the recording's kart 0 has its spear out at
		// the bot on kart 1, by slot.
		adv(CountdownLen + 3*time.Second)
		spears := 0
		for i, r := 0, c.last(TypeRace); i < int(r[9]); i++ {
			e := r[10+(3+demo.KartLen)*i:]
			if e[3+demo.KartSeqs]&15 == 0 {
				continue
			}
			spears++
			if e[0] != d.slotOf[0] || e[3+demo.KartTarget] != d.slotOf[1] {
				t.Fatalf("race %d: slot %d's spear targets slot %d; karts 0 and 1 are slots %d and %d", race, e[0],
					e[3+demo.KartTarget], d.slotOf[0], d.slotOf[1])
			}
		}
		if spears != 1 {
			t.Fatalf("race %d: %d spears in the relay", race, spears)
		}
		until(race, StateResults, CountdownLen+12*time.Second)
		if r := c.last(TypeRace); r == nil || r[9] != MaxPlayers {
			t.Fatalf("race %d: spectator race packet %v", race, r)
		}
		for _, p := range l.Slots {
			if p.LastPlace == 0 {
				t.Fatalf("race %d: %s has no place", race, p.Name)
			}
		}
		until(race, StateWaiting, ResultsLen+2*time.Second)
	}
	for i := 1; i < len(tracks); i++ {
		if tracks[i] == tracks[i-1] {
			t.Fatalf("the same track twice in a row: %v", tracks)
		}
	}
	// The bots never time out.
	h.advance(PlayerTimeout * 2)
	if l.count() != MaxPlayers {
		t.Fatal("bots left")
	}
	for _, o := range h.out {
		if !o.to.IsValid() {
			t.Fatal("sent a packet to a bot")
		}
	}
}
