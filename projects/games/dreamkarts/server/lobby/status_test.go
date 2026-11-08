package lobby

import (
	"net/netip"
	"testing"
	"time"
)

func TestSnapshot(t *testing.T) {
	now := time.Unix(1800000000, 0)
	s := New(func(netip.AddrPort, []byte) {}, now)
	s.Logf = t.Logf
	for i, name := range []string{"RICH", "PAL", "AT-BOT"} {
		p := &Player{Token: uint32(i + 1), Name: name, Lobby: 2, Slot: i, LastSeen: now}
		s.players[p.Token] = p
		s.Lobbies[2].Slots[i] = p
	}
	s.players[9] = &Player{Token: 9, Name: "BROWSER", Lobby: noLobby, Slot: -1, LastSeen: now}
	s.Lobbies[2].State, s.Lobbies[2].Track, s.Lobbies[2].Laps = StateRacing, 0, 5
	snap := s.Snapshot(now)
	l := snap.Lobbies[2]
	if snap.Online != 3 || len(snap.Lobbies) != MaxLobbies || l.ID != 3 || len(l.Players) != 2 ||
		l.Status != "Racing" || l.Detail != "Emerald Beach, 5 laps" || snap.Lobbies[0].Status != "Empty" {
		t.Errorf("snapshot %+v, lobby 3 %+v", snap, l)
	}
}
