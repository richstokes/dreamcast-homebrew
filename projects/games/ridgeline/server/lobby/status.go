package lobby

import (
	"fmt"
	"time"

	"github.com/richstokes/dreamcast-homebrew/projects/common/gameserver/alerts"
	"github.com/richstokes/dreamcast-homebrew/projects/common/gameserver/status"
)

// Snapshot is the server as dcvmu.com/lobbies shows it: who is on the
// mountain in each lobby, their trophies and the time of day there. Test
// clients are left out.
func (s *Server) Snapshot(now time.Time) status.Snapshot {
	snap := status.Snapshot{Game: "Ridgeline", Build: int(s.Latest), Updated: now.Unix()}
	for _, p := range s.players {
		if !alerts.IsTestName(p.Name) {
			snap.Online++
		}
	}
	for _, l := range s.Lobbies {
		e := status.Lobby{ID: l.ID + 1, Max: MaxPlayers, Status: "Empty"}
		for _, p := range l.Slots {
			if p != nil && !alerts.IsTestName(p.Name) {
				e.Players = append(e.Players, status.Player{Name: p.Name, Trophy: p.Summit})
			}
		}
		if len(e.Players) > 0 {
			h := l.Hour(now)
			e.Status, e.Detail = "Exploring", fmt.Sprintf("%02d:%02d on the mountain", int(h), int(h*60)%60)
		}
		snap.Lobbies = append(snap.Lobbies, e)
	}
	return snap
}
