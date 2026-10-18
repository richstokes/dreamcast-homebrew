package lobby

import (
	"encoding/binary"
	"log"
	"net/netip"
	"sort"
	"time"
)

// Timing.
const (
	PlayerTimeout    = 10 * time.Second
	CountdownLen     = 4 * time.Second
	FinishGrace      = 30 * time.Second // after the first finisher
	MaxRaceLen       = 15 * time.Minute // safety net
	ResultsLen       = 8 * time.Second
	LobbyInterval    = 250 * time.Millisecond
	RaceInterval     = 83 * time.Millisecond
	BrowseTimeout    = 30 * time.Second // a console on the lobby list
	MaxSessions      = 300
	maxPacketsPerSec = 80
)

// Points for places 1-6.
var PlacePoints = [MaxPlayers]uint16{10, 8, 6, 4, 2, 1}

// Valid lap counts and limits the console offers.
var validLaps = map[byte]bool{3: true, 5: true, 10: true}

const (
	maxTrack = 16 // tracks are validated loosely; the console clamps
	ccCount  = 3
	colours  = 8
	noLobby  = -1
)

// Player is one console session.
type Player struct {
	Token    uint32
	Addr     netip.AddrPort
	Name     string
	Build    uint16
	Lobby    int
	Slot     int
	JoinSeq  uint64
	LastSeen time.Time
	PingMs   uint16

	Spectating bool // watching Lobby, not in a slot
	Bot        bool // a demo-lobby bot: no address, never times out

	Colour     byte
	Points     uint16
	LastPlace  byte
	StartNonce byte // newest seen from this console

	// Race state.
	Racing   bool
	Grid     byte
	Kart     [KartLen]byte
	KartAt   time.Time
	HasKart  bool
	Finished bool
	FinishMs uint32
	Progress float64 // laps * 100000 + lap distance, for unfinished places

	rateWindow time.Time
	rateCount  int
}

// Lobby is one of the ten rooms.
type Lobby struct {
	ID        int
	Slots     [MaxPlayers]*Player
	Watchers  []*Player // spectators, any number (bounded by MaxSessions)
	Demo      bool      // the bots' demo lobby
	Leader    int       // slot, -1 when empty
	Track     byte
	Laps      byte
	CC        byte
	State     byte
	RaceID    byte
	Green     time.Time // countdown target / race start
	FirstFin  time.Time
	ResultsTo time.Time
	Nonce     byte // newest start nonce accepted from the leader
	synced    bool // the current leader's nonce has been seen once
	lastSent  time.Time
	dirty     bool
}

// Server holds every session and lobby. It is not safe for concurrent use:
// one goroutine feeds it packets and ticks.
type Server struct {
	Latest, Min uint16
	Lobbies     [MaxLobbies]*Lobby
	players     map[uint32]*Player
	joinSeq     uint64
	epoch       time.Time
	send        func(netip.AddrPort, []byte)
	demo        *demoLobby
	Logf        func(string, ...any)
}

// New makes a server that writes packets through send.
func New(send func(netip.AddrPort, []byte), now time.Time) *Server {
	s := &Server{Latest: Build, Min: MinBuild, players: map[uint32]*Player{}, epoch: now, Logf: log.Printf}
	s.send = func(to netip.AddrPort, b []byte) {
		if to.IsValid() { // bots have no address
			send(to, b)
		}
	}
	for i := range s.Lobbies {
		s.Lobbies[i] = &Lobby{ID: i, Leader: -1, Track: 0, Laps: 3, CC: 1}
	}
	return s
}

func (s *Server) ms(now time.Time) uint32 { return uint32(now.Sub(s.epoch) / time.Millisecond) }

// Players returns the number of live sessions.
func (s *Server) Players() int { return len(s.players) }

// Handle processes one datagram.
func (s *Server) Handle(from netip.AddrPort, pkt []byte, now time.Time) {
	h, body, err := ParseClient(pkt)
	if err != nil {
		if len(pkt) >= 4 && pkt[0] == 'D' && pkt[1] == 'K' && pkt[2] != Proto && pkt[3] == TypeHello {
			s.reject(from, RejectTooOld, now)
		}
		return
	}
	p := s.players[h.Token]
	if h.Type == TypeHello {
		s.hello(from, h, body, p, now)
		return
	}
	if p == nil {
		s.reject(from, RejectSession, now)
		return
	}
	if !s.allow(p, now) {
		return
	}
	p.Addr = from // a redialled modem keeps its seat
	p.LastSeen = now
	s.updatePing(p, h, now)
	switch h.Type {
	case TypeList:
		s.sendLobbies(p, now)
	case TypeJoin:
		if len(body) >= 2 && body[1]&JoinSpectate != 0 {
			s.spectate(p, int(body[0]), now)
		} else if len(body) >= 1 {
			s.join(p, int(body[0]), now)
		}
	case TypeLeave:
		s.leave(p, now)
	case TypeWish:
		if len(body) >= 5 {
			s.wish(p, body, now)
		}
	case TypeKart:
		if len(body) >= 26 {
			s.kart(p, body, now)
		}
	case TypePing:
	}
}

func (s *Server) allow(p *Player, now time.Time) bool {
	if now.Sub(p.rateWindow) >= time.Second {
		p.rateWindow = now
		p.rateCount = 0
	}
	p.rateCount++
	return p.rateCount <= maxPacketsPerSec
}

func (s *Server) updatePing(p *Player, h ClientHeader, now time.Time) {
	if h.Echo == 0 {
		return
	}
	rtt := int64(s.ms(now)) - int64(h.Echo) - int64(h.Hold)
	if rtt < 0 || rtt > 5000 {
		return
	}
	if p.PingMs == 0 {
		p.PingMs = uint16(rtt)
	} else {
		p.PingMs = uint16((int64(p.PingMs)*3 + rtt) / 4)
	}
}

func (s *Server) hello(from netip.AddrPort, h ClientHeader, body []byte, p *Player, now time.Time) {
	if h.Build < s.Min {
		s.reject(from, RejectTooOld, now)
		return
	}
	if len(body) < NameLen {
		return
	}
	name := CleanName(body[:NameLen])
	if name == "" {
		s.reject(from, RejectBadName, now)
		return
	}
	if p == nil {
		if len(s.players) >= MaxSessions {
			s.reject(from, RejectFull, now)
			return
		}
		p = &Player{Token: h.Token, Lobby: noLobby, Slot: -1}
		s.players[h.Token] = p
		s.Logf("hello %q build %d from %v (%d sessions)", name, h.Build, from, len(s.players))
	}
	p.Addr, p.Name, p.Build, p.LastSeen = from, name, h.Build, now
	status := byte(0)
	if h.Build < s.Latest {
		status = 1
	}
	b := serverHeader(nil, TypeWelcome, s.ms(now))
	b = append(b, status)
	b = binary.LittleEndian.AppendUint16(b, s.Latest)
	b = binary.LittleEndian.AppendUint16(b, s.Min)
	b = append(b, MaxLobbies, MaxPlayers)
	s.send(from, b)
}

func (s *Server) reject(to netip.AddrPort, reason byte, now time.Time) {
	b := serverHeader(nil, TypeReject, s.ms(now))
	b = append(b, reason)
	b = binary.LittleEndian.AppendUint16(b, s.Latest)
	b = binary.LittleEndian.AppendUint16(b, s.Min)
	s.send(to, b)
}

func (s *Server) sendLobbies(p *Player, now time.Time) {
	b := serverHeader(nil, TypeLobbies, s.ms(now))
	b = append(b, MaxLobbies)
	for _, l := range s.Lobbies {
		n := l.count()
		b = append(b, byte(n), l.State, l.Track)
		leader := ""
		if l.Leader >= 0 && l.Slots[l.Leader] != nil {
			leader = l.Slots[l.Leader].Name
		}
		b = appendName(b, leader)
	}
	for _, l := range s.Lobbies {
		b = append(b, l.watcherCount(), l.flags())
	}
	s.send(p.Addr, b)
}

func (l *Lobby) watcherCount() byte {
	if len(l.Watchers) > 255 {
		return 255
	}
	return byte(len(l.Watchers))
}

func (l *Lobby) flags() byte {
	if l.Demo {
		return LobbyDemo
	}
	return 0
}

func (l *Lobby) count() int {
	n := 0
	for _, p := range l.Slots {
		if p != nil {
			n++
		}
	}
	return n
}

func (l *Lobby) colourFree(c byte, except *Player) bool {
	for _, q := range l.Slots {
		if q != nil && q != except && q.Colour == c {
			return false
		}
	}
	return true
}

func (s *Server) join(p *Player, id int, now time.Time) {
	if id < 0 || id >= MaxLobbies {
		return
	}
	if p.Lobby == id && !p.Spectating {
		s.sendLobby(s.Lobbies[id], p, now) // a repeated JOIN gets its answer again
		return
	}
	l := s.Lobbies[id]
	if l.Demo && !p.Bot {
		s.reject(p.Addr, RejectLobbyFul, now)
		return
	}
	slot := -1
	for i, q := range l.Slots {
		if q == nil {
			slot = i
			break
		}
	}
	if slot < 0 {
		s.reject(p.Addr, RejectLobbyFul, now)
		return
	}
	if p.Lobby != noLobby {
		s.leave(p, now)
	}
	s.joinSeq++
	p.Lobby, p.Slot, p.JoinSeq = id, slot, s.joinSeq
	p.Points, p.LastPlace, p.Racing, p.Finished, p.HasKart = 0, 0, false, false, false
	if !l.colourFree(p.Colour, p) {
		for c := byte(0); c < colours; c++ {
			if l.colourFree(c, p) {
				p.Colour = c
				break
			}
		}
	}
	l.Slots[slot] = p
	if l.Leader < 0 {
		l.Leader, l.synced = slot, false
	}
	l.dirty = true
	s.Logf("%q joined lobby %d slot %d", p.Name, id, slot)
	s.sendLobby(l, p, now)
}

// spectate puts p in lobby id's audience: it gets the lobby and race state
// but has no slot, kart or say in the race. Any number can watch, full or
// not, including the demo lobby.
func (s *Server) spectate(p *Player, id int, now time.Time) {
	if id < 0 || id >= MaxLobbies {
		return
	}
	l := s.Lobbies[id]
	if p.Lobby == id && p.Spectating {
		s.sendLobby(l, p, now)
		return
	}
	if p.Lobby != noLobby {
		s.leave(p, now)
	}
	p.Lobby, p.Slot, p.Spectating = id, -1, true
	p.Racing, p.Finished, p.HasKart = false, false, false
	l.Watchers = append(l.Watchers, p)
	l.dirty = true
	s.Logf("%q is watching lobby %d (%d watching)", p.Name, id, len(l.Watchers))
	s.sendLobby(l, p, now)
}

func (s *Server) leave(p *Player, now time.Time) {
	if p.Lobby == noLobby {
		return
	}
	l := s.Lobbies[p.Lobby]
	if p.Spectating {
		for i, q := range l.Watchers {
			if q == p {
				l.Watchers = append(l.Watchers[:i], l.Watchers[i+1:]...)
				break
			}
		}
		p.Lobby, p.Slot, p.Spectating = noLobby, -1, false
		l.dirty = true
		s.Logf("%q stopped watching lobby %d", p.Name, l.ID)
		return
	}
	l.Slots[p.Slot] = nil
	s.Logf("%q left lobby %d", p.Name, l.ID)
	if l.Leader == p.Slot {
		l.Leader = -1
		var best *Player
		for _, q := range l.Slots {
			if q != nil && (best == nil || q.JoinSeq < best.JoinSeq) {
				best = q
			}
		}
		if best != nil {
			// The new leader's current nonce is not a press; their next is.
			l.Leader, l.Nonce, l.synced = best.Slot, best.StartNonce, true
		}
	}
	p.Lobby, p.Slot, p.Racing = noLobby, -1, false
	if l.count() == 0 {
		*l = Lobby{ID: l.ID, Leader: -1, Laps: 3, CC: 1, Track: l.Track, Watchers: l.Watchers, Demo: l.Demo}
	}
	l.dirty = true
}

func (s *Server) wish(p *Player, b []byte, now time.Time) {
	if p.Lobby == noLobby || p.Spectating {
		return
	}
	l := s.Lobbies[p.Lobby]
	colour, track, laps, cc, nonce := b[0], b[1], b[2], b[3], b[4]
	if l.State == StateWaiting && colour < colours && colour != p.Colour && l.colourFree(colour, p) {
		p.Colour = colour
		l.dirty = true
	}
	if l.Leader != p.Slot {
		p.StartNonce = nonce
		return
	}
	if !l.synced {
		l.Nonce, l.synced, p.StartNonce = nonce, true, nonce
		l.dirty = true
	}
	if l.State == StateWaiting {
		if track < maxTrack && track != l.Track {
			l.Track, l.dirty = track, true
		}
		if validLaps[laps] && laps != l.Laps {
			l.Laps, l.dirty = laps, true
		}
		if cc < ccCount && cc != l.CC {
			l.CC, l.dirty = cc, true
		}
		if nonce != l.Nonce && l.count() >= 2 {
			s.startRace(l, now)
		}
	}
	l.Nonce = nonce
	p.StartNonce = nonce
}

func (s *Server) startRace(l *Lobby, now time.Time) {
	var racers []*Player
	for _, q := range l.Slots {
		if q != nil {
			racers = append(racers, q)
		}
	}
	// Grid: most points at the back (a little handicap), then join order.
	sort.SliceStable(racers, func(i, j int) bool {
		if racers[i].Points != racers[j].Points {
			return racers[i].Points < racers[j].Points
		}
		return racers[i].JoinSeq < racers[j].JoinSeq
	})
	l.RaceID++
	if l.RaceID == 0 {
		l.RaceID = 1
	}
	for i, q := range racers {
		q.Racing, q.Grid = true, byte(i)
		q.Finished, q.FinishMs, q.HasKart, q.Progress = false, 0, false, 0
	}
	l.State = StateCountdown
	l.Green = now.Add(CountdownLen)
	l.FirstFin = time.Time{}
	l.dirty = true
	s.Logf("lobby %d race %d: %d racers, track %d, %d laps, cc %d", l.ID, l.RaceID, len(racers), l.Track, l.Laps, l.CC)
}

func (s *Server) kart(p *Player, b []byte, now time.Time) {
	if p.Lobby == noLobby || !p.Racing {
		return
	}
	l := s.Lobbies[p.Lobby]
	if b[0] != l.RaceID {
		return
	}
	s.takeKart(l, p, b[1:1+KartLen], now, now)
}

// takeKart records a racer's kart (the 21 relayed bytes, flags to race
// time) as it was at time at.
func (s *Server) takeKart(l *Lobby, p *Player, k []byte, at, now time.Time) {
	if l.State != StateCountdown && l.State != StateRacing && l.State != StateResults {
		return
	}
	copy(p.Kart[:], k[:KartLen])
	p.KartAt, p.HasKart = at, true
	lap := k[13]
	dist := binary.LittleEndian.Uint16(k[15:])
	raceMs := binary.LittleEndian.Uint32(k[17:])
	if !p.Finished {
		p.Progress = float64(lap)*100000 + float64(dist)
	}
	if k[0]&KartFinished != 0 && !p.Finished && l.State == StateRacing {
		p.Finished, p.FinishMs = true, raceMs
		if l.FirstFin.IsZero() {
			l.FirstFin = now
		}
		l.dirty = true
		s.Logf("lobby %d: %q finished in %d ms", l.ID, p.Name, raceMs)
	}
}

// Tick advances timers and sends lobby state. Call it often (~every 20 ms).
// Karts are not relayed here: call Relay every RaceInterval from a timer of
// its own, so they go out on a steady beat that consoles time their kart
// packets to (a 20 ms tick could only manage every 100 ms).
func (s *Server) Tick(now time.Time) {
	for tok, p := range s.players {
		limit := PlayerTimeout
		if p.Lobby == noLobby {
			limit = BrowseTimeout
		}
		if now.Sub(p.LastSeen) > limit {
			s.leave(p, now)
			delete(s.players, tok)
			s.Logf("%q timed out", p.Name)
		}
	}
	s.tickDemo(now)
	for _, l := range s.Lobbies {
		s.advance(l, now)
		if l.dirty || now.Sub(l.lastSent) >= LobbyInterval {
			for _, p := range l.Slots {
				if p != nil {
					s.sendLobby(l, p, now)
				}
			}
			for _, p := range l.Watchers {
				s.sendLobby(l, p, now)
			}
			l.lastSent, l.dirty = now, false
		}
	}
}

// Relay sends every active lobby's karts to its consoles and watchers.
func (s *Server) Relay(now time.Time) {
	for _, l := range s.Lobbies {
		if l.State != StateWaiting {
			s.sendRace(l, now)
		}
	}
}

func (s *Server) racers(l *Lobby) []*Player {
	var r []*Player
	for _, p := range l.Slots {
		if p != nil && p.Racing {
			r = append(r, p)
		}
	}
	return r
}

func (s *Server) advance(l *Lobby, now time.Time) {
	switch l.State {
	case StateCountdown:
		if len(s.racers(l)) == 0 {
			s.toWaiting(l)
		} else if !now.Before(l.Green) {
			l.State, l.dirty = StateRacing, true
		}
	case StateRacing:
		r := s.racers(l)
		done := len(r) > 0
		for _, p := range r {
			if !p.Finished {
				done = false
			}
		}
		grace := !l.FirstFin.IsZero() && now.Sub(l.FirstFin) >= FinishGrace
		if done || grace || len(r) == 0 || now.Sub(l.Green) > MaxRaceLen {
			s.finishRace(l, now)
		}
	case StateResults:
		if !now.Before(l.ResultsTo) {
			s.toWaiting(l)
		}
	}
}

func (s *Server) finishRace(l *Lobby, now time.Time) {
	r := s.racers(l)
	sort.SliceStable(r, func(i, j int) bool {
		a, b := r[i], r[j]
		if a.Finished != b.Finished {
			return a.Finished
		}
		if a.Finished {
			return a.FinishMs < b.FinishMs
		}
		return a.Progress > b.Progress
	})
	for i, p := range r {
		p.LastPlace = byte(i + 1)
		if i < len(PlacePoints) {
			p.Points += PlacePoints[i]
		}
	}
	l.State, l.ResultsTo, l.dirty = StateResults, now.Add(ResultsLen), true
	s.Logf("lobby %d race %d over", l.ID, l.RaceID)
}

func (s *Server) toWaiting(l *Lobby) {
	l.State, l.dirty = StateWaiting, true
	for _, p := range l.Slots {
		if p != nil {
			p.Racing, p.Finished, p.HasKart = false, false, false
		}
	}
}

func (s *Server) sendLobby(l *Lobby, p *Player, now time.Time) {
	b := serverHeader(make([]byte, 0, 160), TypeLobby, s.ms(now))
	you := byte(p.Slot)
	if p.Spectating {
		you = SpectatorSlot
	}
	b = append(b, byte(l.ID), l.State, you, byte(int8(l.Leader)), l.Track, l.Laps, l.CC, l.RaceID)
	var t int32
	if l.State == StateCountdown || l.State == StateRacing {
		t = int32(l.Green.Sub(now) / time.Millisecond)
	}
	b = binary.LittleEndian.AppendUint32(b, uint32(t))
	b = append(b, l.Nonce, byte(l.count()))
	for _, q := range l.Slots {
		if q == nil {
			continue
		}
		var flags byte
		if q.Slot == l.Leader {
			flags |= 1
		}
		if q.Racing {
			flags |= 2
		}
		if q.Finished {
			flags |= 4
		}
		b = append(b, byte(q.Slot))
		b = appendName(b, q.Name)
		b = append(b, q.Colour, flags)
		b = binary.LittleEndian.AppendUint16(b, q.PingMs)
		b = binary.LittleEndian.AppendUint16(b, q.Points)
		b = append(b, q.LastPlace, q.Grid)
	}
	b = append(b, l.watcherCount(), l.flags())
	s.send(p.Addr, b)
}

func (s *Server) sendRace(l *Lobby, now time.Time) {
	r := s.racers(l)
	b := serverHeader(make([]byte, 0, 160), TypeRace, s.ms(now))
	n := 0
	for _, p := range r {
		if p.HasKart {
			n++
		}
	}
	b = append(b, l.RaceID, byte(n))
	for _, p := range r {
		if !p.HasKart {
			continue
		}
		age := now.Sub(p.KartAt) / time.Millisecond
		if age < 0 { // handled after this tick's time was taken
			age = 0
		}
		if age > 65535 {
			age = 65535
		}
		b = append(b, byte(p.Slot))
		b = binary.LittleEndian.AppendUint16(b, uint16(age))
		b = append(b, p.Kart[:]...)
	}
	for _, p := range l.Slots {
		if p != nil {
			s.send(p.Addr, b)
		}
	}
	for _, p := range l.Watchers {
		s.send(p.Addr, b)
	}
}
