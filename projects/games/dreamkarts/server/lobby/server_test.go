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
	c.send(TypeWish, []byte{colour, track, laps, cc, c.nonce, WishItems})
}

// wishItems is a build 11 wish with the power-ups switch set as given.
func (c *client) wishItems(colour, track, laps, cc byte, items bool) {
	var f byte
	if items {
		f = WishItems
	}
	c.send(TypeWish, []byte{colour, track, laps, cc, c.nonce, f})
}

func (c *client) kart(raceID, flags, lap byte, dist uint16, raceMs uint32) {
	b := make([]byte, 1+KartLen+4)
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
	if r := b.last(TypeRace); r == nil || r[9] != 2 || len(r) != 10+2*(3+KartLen) {
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

func TestItemsSwitch(t *testing.T) {
	h := newHarness(t)
	a, b := h.client(1, 11), h.client(2, 22)
	a.hello("ann")
	b.hello("bob")
	a.send(TypeJoin, []byte{4})
	b.send(TypeJoin, []byte{4})
	h.advance(300 * time.Millisecond)
	// Items are on until the leader says otherwise; a build 10 wish (no
	// flags byte) leaves the switch alone.
	if l := b.last(TypeLobby); l == nil || l[len(l)-1] != 1 {
		t.Fatalf("items not on by default: %v", l)
	}
	a.send(TypeWish, []byte{0, 0, 3, 1, a.nonce})
	h.advance(300 * time.Millisecond)
	if l := b.last(TypeLobby); l[len(l)-1] != 1 {
		t.Fatalf("a short wish changed the switch: %v", l)
	}
	b.wishItems(5, 0, 3, 1, false) // not the leader
	h.advance(300 * time.Millisecond)
	if l := a.last(TypeLobby); l[len(l)-1] != 1 {
		t.Fatalf("a non-leader switched items off: %v", l)
	}
	a.wishItems(0, 0, 3, 1, false)
	h.advance(300 * time.Millisecond)
	if l := b.last(TypeLobby); l[len(l)-1] != 0 {
		t.Fatalf("the leader could not switch items off: %v", l)
	}
	a.wishItems(0, 0, 3, 1, true)
	h.advance(300 * time.Millisecond)
	if l := b.last(TypeLobby); l[len(l)-1] != 1 {
		t.Fatalf("the leader could not switch items back on: %v", l)
	}
	// Item, spear and mine bytes are relayed as sent.
	a.nonce++
	a.wishItems(0, 0, 3, 1, true)
	h.advance(CountdownLen + 100*time.Millisecond)
	v := a.lobby()
	k := make([]byte, 1+KartLen+4)
	k[0], k[1], k[14], k[15] = v.raceID, 2, 1, 0x02  // holding a spear
	k[22] = 0x53                                     // spear seq 3, mine seq 5
	k[23], k[24], k[25], k[26] = 0x10, 0x27, 0xf8, 1 // spear at 625 m, d -2, after slot 1
	k[27], k[28], k[29] = 0xa0, 0x0f, 0x08           // mine at 250 m, d 2
	a.send(TypeKart, k)
	h.advance(100 * time.Millisecond)
	r := b.last(TypeRace)
	if r == nil || r[9] != 1 {
		t.Fatalf("race relay %v", r)
	}
	got := r[10+3 : 10+3+KartLen]
	if got[14] != 0x02 || got[21] != 0x53 || got[22] != 0x10 || got[23] != 0x27 || got[24] != 0xf8 || got[25] != 1 ||
		got[26] != 0xa0 || got[27] != 0x0f || got[28] != 0x08 {
		t.Fatalf("item bytes not relayed: %v", got)
	}
}

func TestSoloStartAndLeaderHandover(t *testing.T) {
	h := newHarness(t)
	a, b := h.client(1, 11), h.client(2, 22)
	a.hello("ann")
	b.hello("bob")
	a.send(TypeJoin, []byte{0})
	a.wish(0, 0, 3, 1)
	a.nonce++
	a.wish(0, 0, 3, 1)
	h.advance(40 * time.Millisecond)
	// Alone is enough: a practice race while waiting for company.
	if v := a.lobby(); v.state != StateCountdown || v.players[0].flags&2 == 0 {
		t.Fatalf("did not start alone: %+v", v)
	}
	b.send(TypeJoin, []byte{0})
	b.nonce = 7 // bob's own counter is not a press
	b.wish(1, 0, 3, 1)
	h.advance(40 * time.Millisecond)
	if v := b.lobby(); v.state != StateCountdown || len(v.players) != 2 || v.players[1].flags&2 != 0 {
		t.Fatalf("bob should sit out the running race: %+v", v)
	}
	a.send(TypeLeave, nil)
	h.advance(100 * time.Millisecond)
	v := b.lobby()
	if v.leader != v.you {
		t.Fatalf("bob did not take over: %+v", v)
	}
	if v.state != StateWaiting {
		t.Fatalf("race should end when its only racer leaves: %+v", v)
	}
	b.wish(1, 0, 3, 1)
	h.advance(40 * time.Millisecond)
	if b.lobby().state != StateWaiting {
		t.Fatal("handover started a race")
	}
}

func TestRetireStaysInLobby(t *testing.T) {
	h := newHarness(t)
	a, b := h.client(1, 11), h.client(2, 22)
	a.hello("ann")
	b.hello("bob")
	a.send(TypeJoin, []byte{3})
	b.send(TypeJoin, []byte{3})
	a.wish(0, 0, 3, 1)
	b.wish(1, 0, 3, 1)
	a.nonce++
	a.wish(0, 0, 3, 1)
	h.advance(CountdownLen + 100*time.Millisecond)
	id := a.lobby().raceID
	a.kart(id, 0, 1, 100, 5000)
	b.kart(id, 0, 1, 50, 5000)
	h.advance(RaceInterval + 20*time.Millisecond)
	if r := b.last(TypeRace); r == nil || r[9] != 2 {
		t.Fatalf("expected both karts relayed, got %v", r)
	}
	// A stale race id is ignored; the current one takes bob out.
	b.send(TypeRetire, []byte{id + 1})
	h.advance(40 * time.Millisecond)
	if v := b.lobby(); v.players[1].flags&2 == 0 {
		t.Fatalf("old race id retired bob: %+v", v)
	}
	b.send(TypeRetire, []byte{id})
	b.send(TypeRetire, []byte{id}) // the console resends until the state shows it
	h.advance(RaceInterval + 20*time.Millisecond)
	v := b.lobby()
	if v.state != StateRacing || len(v.players) != 2 || v.players[1].flags&2 != 0 || v.players[0].flags&2 == 0 {
		t.Fatalf("bob should be in the lobby but out of the race: %+v", v)
	}
	if r := b.last(TypeRace); r == nil || r[9] != 1 || r[10] != 0 {
		t.Fatalf("only ann's kart should be relayed now, got %v", r)
	}
	b.kart(id, 0, 2, 100, 20000) // a late kart from bob changes nothing
	a.kart(id, KartFinished, 4, 0, 90000)
	h.advance(time.Second)
	v = a.lobby()
	if v.state != StateResults {
		t.Fatalf("race should end when the last racer finishes: %+v", v)
	}
	if v.players[0].place != 1 || v.players[0].points != 10 || v.players[1].place != 0 || v.players[1].points != 0 {
		t.Fatalf("places/points %+v", v.players)
	}
	h.advance(ResultsLen + 100*time.Millisecond)
	// Everyone retiring ends the race with no results.
	a.nonce++
	a.wish(0, 0, 3, 1)
	h.advance(CountdownLen + 100*time.Millisecond)
	id = a.lobby().raceID
	a.send(TypeRetire, []byte{id})
	b.send(TypeRetire, []byte{id})
	h.advance(100 * time.Millisecond)
	if v := a.lobby(); v.state != StateWaiting || v.players[0].points != 10 {
		t.Fatalf("expected the lobby waiting with points kept: %+v", v)
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
