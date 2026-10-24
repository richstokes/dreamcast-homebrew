package lobby

import (
	"os"
	"reflect"
	"regexp"
	"testing"
	"time"

	"github.com/richstokes/dreamcast-homebrew/projects/games/dreamkarts/server/demo"
)

func (h *harness) events() *[]Event {
	var evs []Event
	h.s.OnEvent = func(e Event, _ time.Time) { evs = append(evs, e) }
	return &evs
}

func TestEvents(t *testing.T) {
	h := newHarness(t)
	evs := h.events()
	a, b := h.client(1, 11), h.client(2, 22)
	a.hello("ann")
	b.hello("bob")

	// A race alone is practice: the join is news, the race is not.
	a.send(TypeJoin, []byte{2})
	a.wish(0, 1, 5, 2)
	a.nonce++
	a.wish(0, 1, 5, 2)
	h.advance(40 * time.Millisecond)
	if v := a.lobby(); v.state != StateCountdown {
		t.Fatalf("solo race not counting down: %+v", v)
	}
	want := []Event{{Kind: EventJoin, Lobby: 3, Name: "ANN", Players: []string{"ANN"}}}
	if !reflect.DeepEqual(*evs, want) {
		t.Fatalf("after a solo race: %+v", *evs)
	}

	// Both into lobby 5; the race with company is news.
	*evs = nil
	a.send(TypeJoin, []byte{4})
	b.send(TypeJoin, []byte{4})
	a.wish(0, 6, 10, 0)
	a.nonce++
	a.wish(0, 6, 10, 0)
	h.advance(40 * time.Millisecond)
	want = []Event{
		{Kind: EventJoin, Lobby: 5, Name: "ANN", Players: []string{"ANN"}},
		{Kind: EventJoin, Lobby: 5, Name: "BOB", Players: []string{"ANN", "BOB"}},
		{Kind: EventRace, Lobby: 5, Players: []string{"ANN", "BOB"}, Track: 6, Laps: 10, CC: 0, Items: true},
	}
	if !reflect.DeepEqual(*evs, want) {
		t.Fatalf("got  %+v\nwant %+v", *evs, want)
	}

	// A repeated JOIN (its answer was lost) is not a second join.
	*evs = nil
	b.send(TypeJoin, []byte{4})
	if len(*evs) != 0 {
		t.Fatalf("repeated JOIN made %+v", *evs)
	}
}

// The demo lobby's bots join and race all day without a word.
func TestDemoLobbyMakesNoEvents(t *testing.T) {
	h := newHarness(t)
	evs := h.events()
	races := []*demo.Race{fakeRace(t, 0, 5, 1), fakeRace(t, 2, 6, 3)}
	h.s.EnableDemo(9, races, 1, h.now)
	first := h.s.Lobbies[9].RaceID
	h.advance(3 * time.Minute)
	if h.s.Lobbies[9].RaceID == first {
		t.Fatal("the demo lobby never raced")
	}
	if len(*evs) != 0 {
		t.Fatalf("demo lobby made %+v", *evs)
	}
}

func TestTrackNamesMatchConsole(t *testing.T) {
	src, err := os.ReadFile("../../src/track.c")
	if err != nil {
		t.Fatal(err)
	}
	m := regexp.MustCompile(`(?s)track_names\[TRACK_COUNT\] = \{(.*?)\};`).FindSubmatch(src)
	if m == nil {
		t.Fatal("track_names not found in src/track.c")
	}
	var names []string
	for _, q := range regexp.MustCompile(`"([^"]*)"`).FindAllSubmatch(m[1], -1) {
		names = append(names, string(q[1]))
	}
	if !reflect.DeepEqual(names, TrackNames) {
		t.Errorf("src/track.c has %q, lobby.TrackNames %q", names, TrackNames)
	}
}
