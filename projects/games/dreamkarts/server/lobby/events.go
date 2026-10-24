package lobby

import (
	"strings"
	"time"
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
// events_test.go checks the tracks.
var (
	TrackNames = []string{"EMERALD BEACH", "CORAL CROSSING", "PALM HARBOUR", "TEMPLE HILL", "NEON SKYWAY", "CYBERSTONE",
		"SPIRAL CANYON", "TSUDOKEN DRIFT"}
	CCNames = []string{"50cc", "100cc", "150cc"}
)

// IsTestName says whether a player name belongs to our automated testing:
// "AT" alone or followed by a digit or a dash. Online autotest consoles
// call themselves AT plus four digits and dk-bot defaults to AT-BOT; name
// any other test client AT-SOMETHING. Real names like ATOM still count.
func IsTestName(name string) bool {
	if !strings.HasPrefix(name, "AT") {
		return false
	}
	rest := name[2:]
	return rest == "" || rest[0] == '-' || (rest[0] >= '0' && rest[0] <= '9')
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
