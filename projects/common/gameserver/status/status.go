// Package status publishes a game server's lobbies as JSON on a loopback
// HTTP port, for dcvmu.com/lobbies (DCVMU runs on the same host). The
// server's one goroutine builds a Snapshot every second or so and calls
// Publish; the HTTP handler only ever reads the last one, so the server
// itself is never touched from another goroutine.
package status

import (
	"encoding/json"
	"log"
	"net/http"
	"sync/atomic"
	"time"
)

// Player is someone in a lobby.
type Player struct {
	Name   string `json:"name"`
	Trophy bool   `json:"trophy,omitempty"` // Ridgeline: reached the summit this session
}

// Lobby is one room. Status is a word for its state ("Waiting",
// "Racing", "Exploring"); Detail says more (the track, the time of day).
type Lobby struct {
	ID       int      `json:"id"` // 1-10, as the console shows it
	Max      int      `json:"max"`
	Status   string   `json:"status"`
	Detail   string   `json:"detail,omitempty"`
	Demo     bool     `json:"demo,omitempty"` // the server's own bots, watch only
	Watchers int      `json:"watchers,omitempty"`
	Players  []Player `json:"players"`
}

// Snapshot is the whole server. Online counts the people connected, on
// the lobby browser or in a lobby; bots and test clients are left out of
// everything.
type Snapshot struct {
	Game    string  `json:"game"`
	Build   int     `json:"build"`
	Online  int     `json:"online"`
	Lobbies []Lobby `json:"lobbies"`
	Updated int64   `json:"updated"` // unix seconds
}

// Every is how often a server should publish.
const Every = time.Second

// Publisher holds the latest snapshot as JSON.
type Publisher struct {
	cur atomic.Pointer[[]byte]
}

// Publish stores s (call from the server's goroutine).
func (p *Publisher) Publish(s Snapshot) {
	if s.Lobbies == nil {
		s.Lobbies = []Lobby{}
	}
	for i := range s.Lobbies {
		if s.Lobbies[i].Players == nil {
			s.Lobbies[i].Players = []Player{}
		}
	}
	b, err := json.Marshal(s)
	if err != nil {
		log.Printf("status: %v", err)
		return
	}
	p.cur.Store(&b)
}

// ServeHTTP answers GET /lobbies.json with the latest snapshot.
func (p *Publisher) ServeHTTP(w http.ResponseWriter, r *http.Request) {
	if r.URL.Path != "/lobbies.json" || r.Method != http.MethodGet {
		http.NotFound(w, r)
		return
	}
	b := p.cur.Load()
	if b == nil {
		http.Error(w, "starting", http.StatusServiceUnavailable)
		return
	}
	w.Header().Set("Content-Type", "application/json")
	w.Header().Set("Cache-Control", "no-store")
	w.Write(*b)
}

// Listen serves on addr (a loopback address such as 127.0.0.1:8093) in the
// background; an empty addr serves nothing. A failure is logged, not
// fatal: the game matters more than its web page.
func (p *Publisher) Listen(addr string) {
	if addr == "" {
		return
	}
	srv := &http.Server{Addr: addr, Handler: p, ReadHeaderTimeout: 5 * time.Second}
	go func() {
		log.Printf("status: http://%s/lobbies.json", addr)
		if err := srv.ListenAndServe(); err != nil {
			log.Printf("status: %v", err)
		}
	}()
}
