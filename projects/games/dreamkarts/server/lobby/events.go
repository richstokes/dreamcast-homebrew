package lobby

import "time"

// EventKind says what an Event is about.
type EventKind int

const (
	EventJoin EventKind = iota // a player took a slot in a lobby
	EventRace                  // a race with two or more racers is counting down
)

// Event is lobby news for the outside world (the server's Discord alerts).
// Only people make them: bot and demo lobby activity is left out, and so
// is a race started alone, which is practice and can come every couple of
// minutes.
type Event struct {
	Kind    EventKind
	Lobby   int      // 1-10, as the console shows it
	Name    string   // EventJoin: who joined
	Players []string // EventJoin: everyone in the lobby; EventRace: the racers, grid order
	Track   byte     // EventRace: the race settings
	Laps    byte
	CC      byte
	Items   bool
}

// TrackNames and CCNames match the console's (src/track.c, src/online.c);
// events_test.go checks the tracks.
var (
	TrackNames = []string{"EMERALD BEACH", "CORAL CROSSING", "PALM HARBOUR", "TEMPLE HILL", "NEON SKYWAY", "CYBERSTONE",
		"SPIRAL CANYON"}
	CCNames = []string{"50cc", "100cc", "150cc"}
)

func (s *Server) emit(e Event, now time.Time) {
	if s.OnEvent != nil {
		s.OnEvent(e, now)
	}
}

func names(ps []*Player) []string {
	var out []string
	for _, p := range ps {
		if p != nil {
			out = append(out, p.Name)
		}
	}
	return out
}
