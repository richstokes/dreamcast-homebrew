package lobby

import (
	"time"

	"github.com/richstokes/dreamcast-homebrew/projects/common/gameserver/alerts"
)

// EventKind says what an Event is about.
type EventKind int

const (
	EventJoin EventKind = iota // a player took a slot in a lobby
	EventRace                  // a race with two or more racers is counting down
)

// Event is lobby news for the outside world (the server's Discord and Bark alerts).
// Only people make them: the demo lobby (lobby 10), its bots and anything
// with a test name in it (IsTestName) are left out, and so is a race
// started alone, which is practice and can come every couple of minutes.
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
// DisabledTracks are the tracks the console's race menus no longer offer
// (src/track.h TRACK_DISABLED), which the demo lobby leaves out of its
// rotation: Cyberstone, and the Night Meet lot, which the online lobby does
// offer (as a Free Roam session, RoamTrack) but no recording could race.
// events_test.go checks both against the console's source.
var (
	TrackNames = []string{"EMERALD BEACH", "CORAL CROSSING", "PALM HARBOUR", "TEMPLE HILL", "NEON SKYWAY", "CYBERSTONE",
		"SPIRAL CANYON", "TSUDOKEN DRIFT", "APPLE VALLEY", "NIGHT MEET", "BAY CITY HILLS"}
	DisabledTracks = map[byte]bool{5: true, RoamTrack: true} // CYBERSTONE, retired for now; Night Meet, Free Roam's
	CCNames        = []string{"50cc", "100cc", "150cc"}
)

// IsTestName says whether a player name belongs to our automated testing
// (alerts.IsTestName, shared with Ridgeline's server).
func IsTestName(name string) bool {
	return alerts.IsTestName(name)
}

func anyTest(ps []*Player) bool {
	for _, p := range ps {
		if p != nil && IsTestName(p.Name) {
			return true
		}
	}
	return false
}

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
