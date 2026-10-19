package lobby

import (
	"encoding/binary"
	"net/netip"
	"testing"
	"time"
)

type sent struct {
	to   netip.AddrPort
	data []byte
}

type harness struct {
	t         *testing.T
	s         *Server
	now       time.Time
	out       []sent
	lastRelay time.Time
}

func newHarness(t *testing.T) *harness {
	h := &harness{t: t, now: time.Unix(1000, 0)}
	h.s = New(func(to netip.AddrPort, b []byte) { h.out = append(h.out, sent{to, append([]byte(nil), b...)}) }, h.now)
	h.s.Logf = func(string, ...any) {}
	return h
}

type client struct {
	h     *harness
	addr  netip.AddrPort
	token uint32
	build uint16
	nonce byte
}

func (h *harness) client(port uint16, token uint32) *client {
	return &client{h: h, addr: netip.AddrPortFrom(netip.MustParseAddr("10.0.0.1"), port), token: token, build: Build}
}

func (c *client) send(typ byte, body []byte) {
	pkt := AppendClient(nil, ClientHeader{Type: typ, Token: c.token, Build: c.build}, body)
	c.h.s.Handle(c.addr, pkt, c.h.now)
}

func (c *client) hello(name string) { c.send(TypeHello, appendName(nil, name)) }

func (c *client) wish(colour, track, laps, cc byte) {
	c.send(TypeWish, []byte{colour, track, laps, cc, c.nonce})
}

func (c *client) kart(raceID, flags, lap byte, dist uint16, raceMs uint32) {
	b := make([]byte, 26)
	b[0], b[1], b[14] = raceID, flags, lap
	binary.LittleEndian.PutUint16(b[16:], dist)
	binary.LittleEndian.PutUint32(b[18:], raceMs)
	c.send(TypeKart, b)
}

// last returns the newest packet of a type sent to c.
func (c *client) last(typ byte) []byte {
	for i := len(c.h.out) - 1; i >= 0; i-- {
		o := c.h.out[i]
		if o.to == c.addr && o.data[3] == typ {
			return o.data
		}
	}
	return nil
}

type lobbyView struct {
	id, state, you, leader, track, laps, cc, raceID byte
	players                                         []playerView
}

type playerView struct {
	slot          byte
	name          string
	colour, flags byte
	points        uint16
	place         byte
}

func (c *client) lobby() lobbyView {
	b := c.last(TypeLobby)
	if b == nil {
		c.h.t.Fatalf("no LOBBY packet for %v", c.addr)
	}
	v := lobbyView{id: b[8], state: b[9], you: b[10], leader: b[11], track: b[12], laps: b[13], cc: b[14], raceID: b[15]}
	n := int(b[21])
	for i := 0; i < n; i++ {
		e := b[22+i*20:]
		v.players = append(v.players, playerView{slot: e[0], name: CleanName(e[1:12]), colour: e[12], flags: e[13],
			points: binary.LittleEndian.Uint16(e[16:]), place: e[18]})
	}
	return v
}

// raceLeft reads the ms-until-the-race-ends after a LOBBY packet's players.
func raceLeft(b []byte) int32 {
	o := 22 + int(b[21])*20 + 2
	return int32(binary.LittleEndian.Uint32(b[o:]))
}

// advance runs the server's 20 ms ticks and its kart relays up to d from now.
func (h *harness) advance(d time.Duration) {
	for end := h.now.Add(d); h.now.Before(end); {
		h.now = h.now.Add(20 * time.Millisecond)
		h.s.Tick(h.now)
		if h.now.Sub(h.lastRelay) >= RaceInterval {
			h.lastRelay = h.now
			h.s.Relay(h.now)
		}
	}
}

func TestRaceLifecycle(t *testing.T) {
	h := newHarness(t)
	a, b := h.client(1, 11), h.client(2, 22)
	a.hello("ann")
	if w := a.last(TypeWelcome); w == nil || w[8] != 0 {
		t.Fatalf("welcome %v", w)
	}
	b.hello("bob")
	a.send(TypeJoin, []byte{3})
	b.send(TypeJoin, []byte{3})
	v := b.lobby()
	if v.id != 3 || len(v.players) != 2 || v.leader != a.lobby().you {
		t.Fatalf("lobby %+v", v)
	}
	if v.players[0].colour == v.players[1].colour {
		t.Fatal("players share a colour")
	}
	// Only the leader sets the race; the first wish just syncs its nonce.
	a.wish(0, 2, 5, 2)
	b.wish(5, 1, 10, 0)
	h.advance(300 * time.Millisecond)
	v = b.lobby()
	if v.track != 2 || v.laps != 5 || v.cc != 2 || v.state != StateWaiting {
		t.Fatalf("settings %+v", v)
	}
	a.nonce++
	a.wish(0, 2, 5, 2)
	h.advance(40 * time.Millisecond)
	if v = a.lobby(); v.state != StateCountdown {
		t.Fatalf("not counting down: %+v", v)
	}
	h.advance(CountdownLen + 100*time.Millisecond)
	if v = a.lobby(); v.state != StateRacing {
		t.Fatalf("not racing: %+v", v)
	}
	a.kart(v.raceID, 0, 2, 100, 30000)
	b.kart(v.raceID, 0, 1, 900, 30000)
	h.advance(100 * time.Millisecond)
	if r := b.last(TypeRace); r == nil || r[9] != 2 {
		t.Fatalf("race relay %v", r)
	}
	b.kart(v.raceID, KartFinished, 6, 0, 61000)
	a.kart(v.raceID, KartFinished, 6, 0, 62500)
	h.advance(100 * time.Millisecond)
	v = a.lobby()
	if v.state != StateResults {
		t.Fatalf("not in results: %+v", v)
	}
	for _, p := range v.players {
		want := map[string]uint16{"BOB": 10, "ANN": 8}[p.name]
		if p.points != want {
			t.Errorf("%s has %d points, want %d", p.name, p.points, want)
		}
	}
	h.advance(ResultsLen + 100*time.Millisecond)
	if v = a.lobby(); v.state != StateWaiting {
		t.Fatalf("not back in the lobby: %+v", v)
	}
}

func TestStartNeedsTwoAndLeaderHandover(t *testing.T) {
	h := newHarness(t)
	a, b := h.client(1, 11), h.client(2, 22)
	a.hello("ann")
	b.hello("bob")
	a.send(TypeJoin, []byte{0})
	a.wish(0, 0, 3, 1)
	a.nonce++
	a.wish(0, 0, 3, 1)
	h.advance(40 * time.Millisecond)
	if a.lobby().state != StateWaiting {
		t.Fatal("started with one player")
	}
	b.send(TypeJoin, []byte{0})
	b.nonce = 7 // bob's own counter is not a press
	b.wish(1, 0, 3, 1)
	a.send(TypeLeave, nil)
	h.advance(100 * time.Millisecond)
	v := b.lobby()
	if v.leader != v.you {
		t.Fatalf("bob did not take over: %+v", v)
	}
	b.wish(1, 0, 3, 1)
	h.advance(40 * time.Millisecond)
	if b.lobby().state != StateWaiting {
		t.Fatal("handover started a race")
	}
}

func TestOldClientRejected(t *testing.T) {
	h := newHarness(t)
	h.s.Min = 5
	c := h.client(1, 1)
	c.build = 4
	c.hello("old")
	r := c.last(TypeReject)
	if r == nil || r[8] != RejectTooOld {
		t.Fatalf("reject %v", r)
	}
	c.build = 5
	h.s.Latest = 6
	c.hello("newish")
	if w := c.last(TypeWelcome); w == nil || w[8] != 1 {
		t.Fatalf("expected an update-available welcome, got %v", w)
	}
}

func TestUnfinishedPlacesAndTimeout(t *testing.T) {
	h := newHarness(t)
	a, b, c := h.client(1, 1), h.client(2, 2), h.client(3, 3)
	for i, cl := range []*client{a, b, c} {
		cl.hello([]string{"a", "b", "c"}[i])
		cl.send(TypeJoin, []byte{9})
		cl.wish(byte(i), 0, 3, 1)
	}
	a.nonce++
	a.wish(0, 0, 3, 1)
	h.advance(CountdownLen + 100*time.Millisecond)
	id := a.lobby().raceID
	if left := raceLeft(a.last(TypeLobby)); left != 0 {
		t.Fatalf("race end announced before anyone finished: %d ms", left)
	}
	a.kart(id, KartFinished, 4, 0, 90000)
	b.kart(id, 0, 3, 500, 90000)
	c.kart(id, 0, 3, 100, 90000)
	h.advance(5 * time.Second)
	if left := raceLeft(b.last(TypeLobby)); left < 24500 || left > 25500 {
		t.Fatalf("race ends in %d ms, want about 25 s", left)
	}
	// Keep b and c alive through the grace period.
	for i := 0; i < 32; i++ {
		h.advance(time.Second)
		b.send(TypePing, nil)
		c.send(TypePing, nil)
		a.send(TypePing, nil)
	}
	places := map[string]byte{}
	for _, p := range a.lobby().players {
		places[p.name] = p.place
	}
	if places["A"] != 1 || places["B"] != 2 || places["C"] != 3 {
		t.Fatalf("places %v", places)
	}
	// c goes silent and is dropped.
	h.advance(PlayerTimeout + 2*time.Second)
	if n := len(h.s.players); n != 0 {
		t.Fatalf("%d sessions left after everyone went quiet", n)
	}
}
