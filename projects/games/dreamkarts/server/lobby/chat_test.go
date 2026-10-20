package lobby

import (
	"testing"
	"time"
)

func (c *client) say(id byte, text string) {
	c.send(TypeChat, appendChat([]byte{id}, text))
}

// chats returns the chat lines c has received, as "NAME: TEXT".
func (c *client) chats() []string {
	var out []string
	for _, o := range c.h.out {
		if o.to == c.addr && o.data[3] == TypeChatMsg {
			out = append(out, CleanName(o.data[9:9+NameLen])+": "+CleanChat(o.data[9+NameLen+2:]))
		}
	}
	return out
}

func equal(a, b []string) bool {
	if len(a) != len(b) {
		return false
	}
	for i := range a {
		if a[i] != b[i] {
			return false
		}
	}
	return true
}

func TestChat(t *testing.T) {
	h := newHarness(t)
	a, b, w := h.client(1, 11), h.client(2, 22), h.client(3, 33)
	a.hello("ann")
	b.hello("bob")
	w.hello("wat")
	a.send(TypeJoin, []byte{2})
	b.send(TypeJoin, []byte{2})
	w.send(TypeJoin, []byte{2, JoinSpectate})
	a.say(1, "hello there")
	want := []string{"ANN: HELLO THERE"}
	for _, c := range []*client{a, b, w} {
		if got := c.chats(); !equal(got, want) {
			t.Fatalf("%v got %v, want %v", c.addr, got, want)
		}
	}
	// The packet names the slot, colour and id too.
	m := a.last(TypeChatMsg)
	if m[8] != a.lobby().you || m[9+NameLen+1] != 1 {
		t.Fatalf("chat packet %v", m)
	}
	// A resend of the same id (its echo went missing) is answered with the
	// echo again, to the sender only.
	a.say(1, "hello there")
	if got := a.chats(); !equal(got, []string{"ANN: HELLO THERE", "ANN: HELLO THERE"}) {
		t.Fatalf("resend: sender got %v", got)
	}
	if got := b.chats(); !equal(got, want) {
		t.Fatalf("resend: bob got %v", got)
	}
	// Bad words are starred out; a watcher has no say; another lobby hears
	// nothing.
	h.advance(2 * time.Second)
	b.say(1, "you shithead")
	w.say(1, "boo")
	o := h.client(4, 44)
	o.hello("oth")
	o.send(TypeJoin, []byte{5})
	o.say(1, "anyone here?")
	want = []string{"ANN: HELLO THERE", "BOB: YOU ********"}
	if got := b.chats(); !equal(got, want) {
		t.Fatalf("got %v, want %v", got, want)
	}
	if got := o.chats(); !equal(got, []string{"OTH: ANYONE HERE?"}) {
		t.Fatalf("other lobby got %v", got)
	}
	// Rate limit: a burst of three, then one a second.
	h.advance(5 * time.Second)
	for i := byte(10); i < 16; i++ {
		a.say(i, "spam")
	}
	if n := len(b.chats()); n != 2+ChatBurst {
		t.Fatalf("burst let %d through, want %d", n-2, ChatBurst)
	}
	h.advance(1100 * time.Millisecond)
	a.say(20, "later")
	if got := b.chats(); got[len(got)-1] != "ANN: LATER" {
		t.Fatalf("a message after the burst was dropped: %v", got)
	}
}

func TestBadNameRejected(t *testing.T) {
	h := newHarness(t)
	c := h.client(1, 1)
	c.hello("SH1THEAD")
	if r := c.last(TypeReject); r == nil || r[8] != RejectBadName {
		t.Fatalf("reject %v", r)
	}
	c.hello("CLASSY")
	if w := c.last(TypeWelcome); w == nil {
		t.Fatal("an innocent name was refused")
	}
}
