package lobby

import (
	"fmt"
	"strings"
	"time"

	"github.com/richstokes/dreamcast-homebrew/projects/common/gameserver/status"
)

// Snapshot is the server as dcvmu.com/lobbies shows it: who is in each
// lobby and what it is doing. Test clients are left out; the demo lobby's
// bots are named but not counted as online.
func (s *Server) Snapshot(now time.Time) status.Snapshot {
	snap := status.Snapshot{Game: "DreamKarts", Build: int(s.Latest), Updated: now.Unix()}
	for _, p := range s.players {
		if !p.Bot && !IsTestName(p.Name) {
			snap.Online++
		}
	}
	for _, l := range s.Lobbies {
		e := status.Lobby{ID: l.ID + 1, Max: MaxPlayers, Demo: l.Demo}
		for _, w := range l.Watchers {
			if !IsTestName(w.Name) {
				e.Watchers++
			}
		}
		for _, p := range l.Slots {
			if p != nil && !IsTestName(p.Name) {
				e.Players = append(e.Players, status.Player{Name: p.Name})
			}
		}
		track := fmt.Sprintf("Track %d", l.Track+1)
		if int(l.Track) < len(TrackNames) {
			track = titleCase(TrackNames[l.Track])
		}
		switch {
		case len(e.Players) == 0:
			e.Status = "Empty"
		case l.State == StateWaiting:
			e.Status = "Waiting"
		case l.Track == RoamTrack:
			e.Status, e.Detail = "Free Roam", track
		case l.State == StateResults:
			e.Status, e.Detail = "Results", track
		default:
			e.Status, e.Detail = "Racing", fmt.Sprintf("%s, %d laps", track, l.Laps)
		}
		snap.Lobbies = append(snap.Lobbies, e)
	}
	return snap
}

// titleCase turns the console's "EMERALD BEACH" into "Emerald Beach".
func titleCase(s string) string {
	words := strings.Fields(strings.ToLower(s))
	for i, w := range words {
		words[i] = strings.ToUpper(w[:1]) + w[1:]
	}
	return strings.Join(words, " ")
}
