package lobby

import (
	"encoding/binary"
	"fmt"
	"math"
	"net/netip"
	"testing"
	"time"

	"github.com/richstokes/dreamcast-homebrew/projects/common/gameserver/wordfilter"
)

type sent struct {
	to   netip.AddrPort
	data []byte
}

type harness struct {
	t    *testing.T
	s    *Server
	now  time.Time
	out  []sent
	toks map[string]uint32
}

func newHarness(t *testing.T) *harness {
	h := &harness{t: t, now: time.Unix(1700000000, 0), toks: map[string]uint32{}}
	h.s = New(func(to netip.AddrPort, b []byte) {
		h.out = append(h.out, sent{to, append([]byte(nil), b...)})
	}, h.now)
	h.s.Filter = wordfilter.New([]string{"blorp"})
	h.s.Logf = func(string, ...any) {}
	return h
}

func addr(name string) netip.AddrPort {
	var b [4]byte
	for i, c := range name {
		b[i%4] += byte(c)
	}
	return netip.AddrPortFrom(netip.AddrFrom4([4]byte{10, b[1], b[2], b[3]}), 5000+uint16(b[0]))
}

func (h *harness) packet(name string, typ byte, build uint16, body []byte) {
	tok, ok := h.toks[name]
	if !ok {
		tok = uint32(len(h.toks) + 100)
		h.toks[name] = tok
	}
	h.s.Handle(addr(name), ClientPacket(typ, tok, 0, 0, build, body), h.now)
}

func (h *harness) hello(name string) {
	b := make([]byte, NameLen)
	PutName(b, name)
	h.packet(name, TypeHello, Build, b)
}

func (h *harness) join(name string, lobby int) {
	h.packet(name, TypeJoin, Build, []byte{byte(lobby), 1, 2})
}

func (h *harness) truck(name string, flags byte) {
	b := make([]byte, TruckLen)
	b[0] = flags
	b[1] = byte(len(name))
	h.packet(name, TypeTruck, Build, b)
}

func (h *harness) advance(d time.Duration) {
	h.now = h.now.Add(d)
	h.s.Tick(h.now)
}

// last returns the newest packet of a type sent to name, or nil.
func (h *harness) last(name string, typ byte) []byte {
	for i := len(h.out) - 1; i >= 0; i-- {
		o := h.out[i]
		if o.to == addr(name) && len(o.data) >= 8 && o.data[3] == typ {
			return o.data
		}
	}
	return nil
}

type playerView struct {
	slot   int
	name   string
	model  byte
	trophy bool
}

func parseLobby(t *testing.T, b []byte) (id, you int, hour float64, players []playerView) {
	if b == nil {
		t.Fatal("no LOBBY packet")
	}
	id, you = int(b[8]), int(b[9])
	hour = float64(binary.LittleEndian.Uint16(b[10:])) / 2000
	n := int(b[12])
	for i := 0; i < n; i++ {
		e := b[13+i*PlayerLen:]
		players = append(players, playerView{int(e[0]), CleanName(e[1:12]), e[12], e[14]&FlagSummit != 0})
	}
	return
}

func TestHelloWelcome(t *testing.T) {
	h := newHarness(t)
	h.hello("RICH")
	w := h.last("RICH", TypeWelcome)
	if w == nil {
		t.Fatal("no WELCOME")
	}
	if w[8] != 0 || binary.LittleEndian.Uint16(w[9:]) != Build || w[13] != MaxLobbies || w[14] != MaxPlayers {
		t.Errorf("WELCOME % x", w)
	}
}

func TestRejects(t *testing.T) {
	h := newHarness(t)
	h.packet("OLD", TypeHello, MinBuild-1, make([]byte, NameLen))
	if r := h.last("OLD", TypeReject); r == nil || r[8] != RejectTooOld {
		t.Errorf("old build: % x", r)
	}
	h.hello("BLORPHEAD")
	if r := h.last("BLORPHEAD", TypeReject); r == nil || r[8] != RejectBadName {
		t.Errorf("bad name: % x", r)
	}
	// Another protocol's hello is told it is too old, in a REJECT it can read.
	pkt := ClientPacket(TypeHello, 7, 0, 0, Build, make([]byte, NameLen))
	pkt[2] = Proto + 1
	h.s.Handle(addr("FUTURE"), pkt, h.now)
	if r := h.last("FUTURE", TypeReject); r == nil || r[8] != RejectTooOld {
		t.Errorf("other protocol: % x", r)
	}
	// A packet from a session the server does not know: say hello again.
	h.packet("STRANGER", TypePing, Build, nil)
	if r := h.last("STRANGER", TypeReject); r == nil || r[8] != RejectSession {
		t.Errorf("unknown session: % x", r)
	}
}

// Players drop in and out whenever they like: no leader, no waiting.
func TestDropInAndOut(t *testing.T) {
	h := newHarness(t)
	for _, n := range []string{"A1", "B2", "C3", "D4", "E5"} {
		h.hello(n)
	}
	for _, n := range []string{"A1", "B2", "C3", "D4"} {
		h.join(n, 3)
	}
	h.advance(LobbyInterval)
	id, you, _, ps := parseLobby(t, h.last("C3", TypeLobby))
	if id != 3 || you != 2 || len(ps) != 4 || ps[2].name != "C3" || ps[2].model != 1 {
		t.Errorf("lobby %d you %d players %+v", id, you, ps)
	}
	h.join("E5", 3)
	if r := h.last("E5", TypeReject); r == nil || r[8] != RejectLobbyFull {
		t.Errorf("fifth player: % x", r)
	}
	// B leaves; E takes the free slot straight away.
	h.packet("B2", TypeLeave, Build, nil)
	h.join("E5", 3)
	h.advance(LobbyInterval)
	_, you, _, ps = parseLobby(t, h.last("E5", TypeLobby))
	if you != 1 || len(ps) != 4 {
		t.Errorf("E5 in slot %d with %d players", you, len(ps))
	}
	// Joining another lobby leaves this one.
	h.join("A1", 5)
	h.advance(LobbyInterval)
	if _, _, _, ps = parseLobby(t, h.last("C3", TypeLobby)); len(ps) != 3 {
		t.Errorf("after A1 moved: %d players", len(ps))
	}
	if id, _, _, _ = parseLobby(t, h.last("A1", TypeLobby)); id != 5 {
		t.Errorf("A1 in lobby %d", id)
	}
}

// The trophy: set by the summit bit in a TRUCK, kept for the session,
// shown in the lobby and the browser.
func TestSummitTrophy(t *testing.T) {
	h := newHarness(t)
	h.hello("RICH")
	h.hello("PAL")
	h.join("RICH", 0)
	h.join("PAL", 0)
	h.truck("RICH", 0)
	h.advance(LobbyInterval)
	if _, _, _, ps := parseLobby(t, h.last("PAL", TypeLobby)); ps[0].trophy {
		t.Error("trophy before the summit")
	}
	h.truck("RICH", TruckSummit)
	h.advance(time.Millisecond)
	if _, _, _, ps := parseLobby(t, h.last("PAL", TypeLobby)); !ps[0].trophy || ps[1].trophy {
		t.Errorf("after the summit: %+v", ps)
	}
	// Sticky: a later truck without the bit, and a move to another lobby.
	h.truck("RICH", 0)
	h.join("RICH", 4)
	h.advance(LobbyInterval)
	if _, _, _, ps := parseLobby(t, h.last("RICH", TypeLobby)); !ps[0].trophy {
		t.Error("trophy lost on moving lobby")
	}
	h.packet("PAL", TypeList, Build, nil)
	b := h.last("PAL", TypeLobbies)
	if b == nil || b[8] != MaxLobbies {
		t.Fatalf("LOBBIES % x", b)
	}
	o := 9
	var names []string
	for i := 0; i < MaxLobbies; i++ {
		n := int(b[o])
		o += 2
		for k := 0; k < n; k++ {
			names = append(names, fmt.Sprintf("%d:%s:%v", i, CleanName(b[o+1:o+12]), b[o]&FlagSummit != 0))
			o += 1 + NameLen
		}
	}
	if o != len(b) || fmt.Sprint(names) != "[0:PAL:false 4:RICH:true]" {
		t.Errorf("browser entries %v (%d of %d bytes)", names, o, len(b))
	}
}

func TestRelay(t *testing.T) {
	h := newHarness(t)
	for _, n := range []string{"A1", "B2", "C3"} {
		h.hello(n)
		h.join(n, 2)
	}
	h.truck("A1", 0)
	h.truck("B2", 0)
	h.now = h.now.Add(30 * time.Millisecond)
	h.s.Relay(h.now)
	b := h.last("A1", TypeTrucks)
	if b == nil || b[8] != 1 || b[9] != 1 || binary.LittleEndian.Uint16(b[10:]) != 30 || b[12+1] != 2 {
		t.Errorf("A1's relay % x", b)
	}
	if b = h.last("C3", TypeTrucks); b == nil || b[8] != 2 || len(b) != 9+2*(3+TruckLen) {
		t.Errorf("C3's relay % x", b)
	}
}

func TestTimeoutsAndDay(t *testing.T) {
	h := newHarness(t)
	h.hello("A1")
	h.join("A1", 0)
	for i := 0; i < 60; i++ { // five minutes, sending its truck
		h.truck("A1", 0)
		h.advance(5 * time.Second)
	}
	_, _, hour, _ := parseLobby(t, h.last("A1", TypeLobby))
	want := math.Mod(StartHour+300/SecondsPerHour, 24)
	if math.Abs(hour-want) > 0.01 {
		t.Errorf("hour %.3f, want %.3f", hour, want)
	}
	h.advance(PlayerTimeout + time.Second)
	if h.s.Players() != 0 || h.s.Lobbies[0].Count() != 0 {
		t.Fatalf("silent player kept: %d sessions", h.s.Players())
	}
	// The next one in gets a fresh morning.
	h.hello("B2")
	h.join("B2", 0)
	h.advance(LobbyInterval)
	if _, _, hour, _ = parseLobby(t, h.last("B2", TypeLobby)); math.Abs(hour-StartHour) > 0.01 {
		t.Errorf("fresh lobby at %.2f h", hour)
	}
}

// Joins reach OnJoin with who is there (trophies marked); test names never do.
func TestJoinEvents(t *testing.T) {
	h := newHarness(t)
	var got []string
	h.s.OnJoin = func(j Join, _ time.Time) { got = append(got, fmt.Sprintf("%s@%d:%v", j.Name, j.Lobby, j.Players)) }
	for _, n := range []string{"RICH", "AT-BOT", "PAL"} {
		h.hello(n)
	}
	h.join("RICH", 2)
	h.truck("RICH", TruckSummit)
	h.join("AT-BOT", 2)
	h.join("PAL", 2)
	h.join("PAL", 2) // its resent JOIN is not news
	want := "[RICH@3:[RICH] PAL@3:[RICH \U0001F3C6 AT-BOT PAL]]"
	if fmt.Sprint(got) != want {
		t.Errorf("joins %v, want %s", got, want)
	}
}

func TestSnapshot(t *testing.T) {
	h := newHarness(t)
	for _, n := range []string{"RICH", "AT-BOT", "PAL"} {
		h.hello(n)
	}
	h.join("RICH", 1)
	h.join("AT-BOT", 1)
	h.truck("RICH", TruckSummit)
	snap := h.s.Snapshot(h.now)
	l := snap.Lobbies[1]
	if snap.Online != 2 || l.ID != 2 || l.Status != "Exploring" || l.Detail != "09:00 on the mountain" ||
		fmt.Sprint(l.Players) != "[{RICH true}]" || snap.Lobbies[0].Status != "Empty" {
		t.Errorf("snapshot %+v, lobby 2 %+v", snap, l)
	}
}

// A console on the truck select only pings: it must hear back, or it
// reports a lost connection after 6 s.
func TestPingAnsweredBetweenLobbies(t *testing.T) {
	h := newHarness(t)
	h.hello("RICH")
	h.out = nil
	h.packet("RICH", TypePing, Build, nil)
	if h.last("RICH", TypeLobbies) == nil {
		t.Error("a ping from the truck select went unanswered")
	}
	h.join("RICH", 0)
	h.out = nil
	h.packet("RICH", TypePing, Build, nil)
	if len(h.out) != 0 {
		t.Errorf("in a lobby a ping got %d packets (its LOBBY packets keep it alive)", len(h.out))
	}
}
