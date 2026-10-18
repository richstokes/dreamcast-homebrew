package lobby

import (
	"math/rand"
	"time"

	"github.com/richstokes/dreamcast-homebrew/projects/games/dreamkarts/server/demo"
)

// The demo lobby: six bots that race forever, replaying recorded all-CPU
// races (each recording gave its six CPUs different personalities). After
// every race the leader picks another track at random. People can watch it
// but never join it, so a stream always has a race to show.
const (
	DemoWait   = 14 * time.Second // in the lobby between races
	DemoCup    = 8                // races before the points start again
	demoFrame  = time.Second / demo.Rate
	demoTokens = 0xd0000000 // bot tokens (never in the session map)
)

var demoNames = []string{"TURBO", "HAN", "ZIPPY", "BLITZ", "BLAZE", "NITRO", "PIXEL", "ROCKET", "SPARKY",
	"DASH", "VORTEX", "BREEZE", "JETT", "MAVERICK", "SUNNY", "BOLT"}

type demoLobby struct {
	l       *Lobby
	byTrack map[byte][]*demo.Race
	tracks  []byte
	bots    []*Player
	race    *demo.Race
	raceID  byte
	kartOf  map[*Player]int
	rng     *rand.Rand
	state   byte
	waitTo  time.Time
	last    *demo.Race
	races   int
}

// EnableDemo fills lobby id (0-based) with bots that replay races.
func (s *Server) EnableDemo(id int, races []*demo.Race, seed int64, now time.Time) {
	if id < 0 || id >= MaxLobbies || len(races) == 0 {
		return
	}
	d := &demoLobby{l: s.Lobbies[id], byTrack: map[byte][]*demo.Race{}, kartOf: map[*Player]int{},
		rng: rand.New(rand.NewSource(seed)), state: 255}
	for _, r := range races {
		if r.Karts < MaxPlayers {
			continue
		}
		if d.byTrack[r.Track] == nil {
			d.tracks = append(d.tracks, r.Track)
		}
		d.byTrack[r.Track] = append(d.byTrack[r.Track], r)
	}
	if len(d.tracks) == 0 {
		return
	}
	d.l.Demo = true
	names := d.rng.Perm(len(demoNames))
	paint := d.rng.Perm(colours)
	for i := 0; i < MaxPlayers; i++ {
		p := &Player{Token: demoTokens + uint32(i), Name: demoNames[names[i]], Build: Build, Lobby: noLobby, Slot: -1,
			Bot: true, Colour: byte(paint[i]), PingMs: uint16(18 + d.rng.Intn(60))}
		s.join(p, id, now)
		d.bots = append(d.bots, p)
	}
	s.demo = d
	s.Logf("demo lobby %d: %d bots, %d races on %d tracks", id, len(d.bots), len(races), len(d.tracks))
}

// pickTrack has the leader choose the next track: never the last one.
func (d *demoLobby) pickTrack() {
	t := d.tracks[d.rng.Intn(len(d.tracks))]
	for len(d.tracks) > 1 && d.last != nil && t == d.last.Track {
		t = d.tracks[d.rng.Intn(len(d.tracks))]
	}
	d.l.Track, d.l.dirty = t, true
	rs := d.byTrack[t]
	d.race = rs[d.rng.Intn(len(rs))]
	d.l.Laps, d.l.CC = d.race.Laps, d.race.CC
}

func (s *Server) tickDemo(now time.Time) {
	d := s.demo
	if d == nil {
		return
	}
	l := d.l
	if l.State != d.state {
		d.state = l.State
		if l.State == StateWaiting {
			if d.races > 0 && d.races%DemoCup == 0 {
				for _, p := range d.bots {
					p.Points, p.LastPlace = 0, 0
				}
				s.Logf("demo lobby %d: a new cup", l.ID)
			}
			d.pickTrack()
			d.waitTo = now.Add(DemoWait)
		}
	}
	switch l.State {
	case StateWaiting:
		if !now.Before(d.waitTo) {
			s.startDemoRace(d, now)
		}
	case StateCountdown, StateRacing, StateResults:
		if l.RaceID != d.raceID || d.race == nil {
			return
		}
		f, at := 0, now
		if t := now.Sub(l.Green); t > 0 {
			f = int(t / demoFrame)
			if f >= d.race.Frames {
				f = d.race.Frames - 1
			}
			at = l.Green.Add(time.Duration(f) * demoFrame)
		}
		for _, p := range d.bots {
			if k, ok := d.kartOf[p]; ok && p.Racing {
				s.takeKart(l, p, d.race.Kart(f, k), at, now)
			}
		}
	}
}

// startDemoRace starts the chosen recording with the bots dealt onto its
// karts at random, so a bot drives differently every race.
func (s *Server) startDemoRace(d *demoLobby, now time.Time) {
	l := d.l
	s.startRace(l, now)
	d.raceID, d.last = l.RaceID, d.race
	d.races++
	order := d.rng.Perm(len(d.bots))
	for i, p := range d.bots {
		d.kartOf[p] = order[i]
		p.Grid = byte(order[i]) // the recording's grid slot
	}
	s.Logf("demo lobby %d: replaying %s", l.ID, d.race.Name)
}
