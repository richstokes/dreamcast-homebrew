package alerts

import (
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"testing"
	"time"
)

// fakeDiscord records every post per path and answers with the queued
// statuses first (then 204).
type fakeDiscord struct {
	*httptest.Server
	mu      sync.Mutex
	posts   map[string][]map[string]any
	answers []int
}

func newFake(t *testing.T) *fakeDiscord {
	f := &fakeDiscord{posts: map[string][]map[string]any{}}
	f.Server = httptest.NewTLSServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		f.mu.Lock()
		defer f.mu.Unlock()
		if len(f.answers) > 0 {
			code := f.answers[0]
			f.answers = f.answers[1:]
			if code == http.StatusBadRequest { // Bark repeats the key back
				w.WriteHeader(code)
				fmt.Fprintf(w, `{"code": 400, "message": "failed to get [%s] device token from database"}`, strings.TrimPrefix(r.URL.Path, "/"))
				return
			}
			if code == http.StatusTooManyRequests {
				w.WriteHeader(code)
				fmt.Fprint(w, `{"message": "You are being rate limited.", "retry_after": 0.05, "global": false}`)
				return
			}
			w.WriteHeader(code)
			return
		}
		b, _ := io.ReadAll(r.Body)
		var m map[string]any
		if err := json.Unmarshal(b, &m); err != nil {
			t.Errorf("bad JSON posted: %v", err)
		}
		f.posts[r.URL.Path] = append(f.posts[r.URL.Path], m)
		w.WriteHeader(http.StatusNoContent)
	}))
	old := Client
	Client = f.Client()
	t.Cleanup(func() { Client = old; f.Close() })
	return f
}

func (f *fakeDiscord) count(path string) int {
	f.mu.Lock()
	defer f.mu.Unlock()
	return len(f.posts[path])
}

type logs struct {
	mu    sync.Mutex
	lines []string
}

func (l *logs) logf(format string, args ...any) {
	l.mu.Lock()
	defer l.mu.Unlock()
	l.lines = append(l.lines, fmt.Sprintf(format, args...))
}

func (l *logs) text() string {
	l.mu.Lock()
	defer l.mu.Unlock()
	return strings.Join(l.lines, "\n")
}

func mustParse(t *testing.T, s string) *Config {
	t.Helper()
	c, err := Parse([]byte(s))
	if err != nil {
		t.Fatal(err)
	}
	return c
}

func TestParseRejectsMistakes(t *testing.T) {
	for _, bad := range []string{
		`{"webhooks": [{"url": "https://x/y", "events": ["lobby_join"]}]}`,
		`{"webhooks": [{"name": "a", "url": "http://x/y", "events": ["lobby_join"]}]}`,
		`{"webhooks": [{"name": "a", "url": "https://x/y", "events": ["lobby_joined"]}]}`,
		`{"webhooks": [{"name": "a", "url": "https://x/y", "events": ["lobby_join"], "throttle": {"lobby_join": "soon"}}]}`,
		`{"webhooks": [{"name": "a", "url": "https://x/y", "events": ["lobby_join"], "throttle": {"race": "1m"}}]}`,
		`{"webhooks": [{"name": "a", "url": "https://x/y"}, {"name": "a", "url": "https://x/z"}]}`,
		`{"webhooks": [{"name": "a", "url": "https://x/y", "colour": "red"}]}`,
		`{"webhooks": [{"name": "a", "type": "slack", "url": "https://x/y"}]}`,
	} {
		if _, err := Parse([]byte(bad)); err == nil {
			t.Errorf("accepted %s", bad)
		}
	}
}

func TestLoadMissingFileIsOff(t *testing.T) {
	n, err := Load(filepath.Join(t.TempDir(), "nope.json"), t.Logf)
	if n != nil || err != nil {
		t.Fatalf("missing file: %v, %v", n, err)
	}
	n.Notify(LobbyJoin, Message{Title: "x", Time: time.Now()}) // nil-safe
	n.Close(time.Second)
	path := filepath.Join(t.TempDir(), "bad.json")
	os.WriteFile(path, []byte(`{"webhooks": [{"name": "a"}]}`), 0o600)
	if _, err := Load(path, t.Logf); err == nil {
		t.Fatal("bad file loaded")
	}
}

// Joins are throttled per webhook, races are not, and each webhook only
// gets the events it asked for.
func TestThrottleAndRouting(t *testing.T) {
	f := newFake(t)
	c := mustParse(t, fmt.Sprintf(`{"webhooks": [
		{"name": "main", "url": "%[1]s/a", "events": ["lobby_join", "race_start"]},
		{"name": "races", "url": "%[1]s/b", "events": ["race_start"]},
		{"name": "chatty", "url": "%[1]s/c", "events": ["lobby_join"], "throttle": {"lobby_join": "1m"}}]}`, f.URL))
	var l logs
	n := New(c, l.logf)
	t0 := time.Unix(1000, 0)
	for i, at := range []time.Duration{0, time.Second, 61 * time.Second, 4 * time.Minute, 5*time.Minute + time.Second} {
		n.Notify(LobbyJoin, Message{Title: fmt.Sprintf("join %d", i), Time: t0.Add(at)})
	}
	for i := 0; i < 5; i++ {
		n.Notify(RaceStart, Message{Title: "race", Time: t0.Add(time.Duration(i) * time.Second)})
	}
	n.Close(5 * time.Second)
	// main: joins at 0 and 5m01s; chatty: 0, 61s, 4m, 5m01s.
	if got := f.count("/a"); got != 2+5 {
		t.Errorf("main got %d posts, want 7", got)
	}
	if got := f.count("/b"); got != 5 {
		t.Errorf("races got %d posts, want 5", got)
	}
	if got := f.count("/c"); got != 4 {
		t.Errorf("chatty got %d posts, want 4", got)
	}
	p := f.posts["/a"][0]
	if p["username"] != "DreamKarts" {
		t.Errorf("username %v", p["username"])
	}
	if am, _ := p["allowed_mentions"].(map[string]any); am == nil || len(am["parse"].([]any)) != 0 {
		t.Errorf("allowed_mentions %v: player names must not ping", p["allowed_mentions"])
	}
	if l.text() != "" {
		t.Errorf("unexpected logs:\n%s", l.text())
	}
}

// A lobby filling up posts every new player; the same player again inside
// the window does not, and closed windows are forgotten.
func TestJoinThrottlePerPlayer(t *testing.T) {
	f := newFake(t)
	n := New(mustParse(t, fmt.Sprintf(`{"webhooks": [{"name": "main", "url": "%s/a", "events": ["lobby_join", "race_start"]}]}`, f.URL)), t.Logf)
	t0 := time.Unix(1000, 0)
	join := func(name string, at time.Duration) {
		n.Notify(LobbyJoin, Message{Title: name + " joined", Time: t0.Add(at), Key: name})
	}
	join("ANN", 0)
	join("BOB", time.Second)
	join("ANN", 30*time.Second) // reconnect: dropped
	join("CAT", time.Minute)
	join("BOB", 4*time.Minute) // hopped lobbies: dropped
	join("ANN", 5*time.Minute+time.Second)
	n.Notify(RaceStart, Message{Title: "race", Time: t0.Add(6 * time.Minute)})
	if got := len(n.hooks[0].last); got != 2 {
		t.Errorf("remembering %d windows at 6m, want 2 (ANN and the race)", got)
	}
	n.Close(5 * time.Second)
	var titles []string
	for _, p := range f.posts["/a"] {
		titles = append(titles, p["embeds"].([]any)[0].(map[string]any)["title"].(string))
	}
	want := "ANN joined, BOB joined, CAT joined, ANN joined, race"
	if got := strings.Join(titles, ", "); got != want {
		t.Errorf("posted %q, want %q", got, want)
	}
}

func TestRateLimitRetry(t *testing.T) {
	f := newFake(t)
	f.answers = []int{http.StatusTooManyRequests}
	var l logs
	n := New(mustParse(t, fmt.Sprintf(`{"webhooks": [{"name": "main", "url": "%s/a", "events": ["race_start"]}]}`, f.URL)), l.logf)
	n.Notify(RaceStart, Message{Title: "race", Time: time.Now()})
	n.Close(5 * time.Second)
	if f.count("/a") != 1 {
		t.Fatalf("got %d posts after a 429, want 1; logs:\n%s", f.count("/a"), l.text())
	}
	if !strings.Contains(l.text(), "rate limited") {
		t.Errorf("429 not logged:\n%s", l.text())
	}
}

// The webhook URL holds its token: errors name the webhook, never the URL.
func TestErrorsDoNotLeakURL(t *testing.T) {
	f := newFake(t)
	f.answers = []int{http.StatusNotFound}
	var l logs
	secret := "/api/webhooks/123/SECRETTOKEN"
	dead := "https://127.0.0.1:1" + secret // nothing listens there
	n := New(mustParse(t, fmt.Sprintf(`{"webhooks": [
		{"name": "gone", "url": "%s%s", "events": ["race_start"]},
		{"name": "dead", "url": "%s", "events": ["race_start"]}]}`, f.URL, secret, dead)), l.logf)
	n.Notify(RaceStart, Message{Title: "race", Time: time.Now()})
	n.Close(5 * time.Second)
	out := l.text()
	if !strings.Contains(out, "alerts gone: HTTP 404") || !strings.Contains(out, "alerts dead:") {
		t.Errorf("errors not logged by name:\n%s", out)
	}
	if strings.Contains(out, "SECRETTOKEN") {
		t.Errorf("log leaks the webhook token:\n%s", out)
	}
}

func TestBarkErrorDoesNotLeakKey(t *testing.T) {
	f := newFake(t)
	f.answers = []int{http.StatusBadRequest}
	var l logs
	n := New(mustParse(t, fmt.Sprintf(`{"webhooks": [{"name": "phone", "type": "bark", "url": "%s/BARKKEY123", "events": ["race_start"]}]}`, f.URL)), l.logf)
	n.Notify(RaceStart, Message{Title: "race", Time: time.Now()})
	n.Close(5 * time.Second)
	out := l.text()
	if !strings.Contains(out, "alerts phone: HTTP 400") || strings.Contains(out, "BARKKEY123") {
		t.Errorf("want a redacted HTTP 400, got:\n%s", out)
	}
}

func TestBarkPayload(t *testing.T) {
	f := newFake(t)
	n := New(mustParse(t, fmt.Sprintf(`{"webhooks": [
		{"name": "phone", "type": "bark", "url": "%s/key", "events": ["race_start"]}]}`, f.URL)), t.Logf)
	n.Notify(RaceStart, Message{Title: "Race starting in lobby 3", Color: 1, Time: time.Now(), Fields: []Field{
		{Name: "Track", Value: "EMERALD BEACH", Inline: true}, {Name: "Laps", Value: "3", Inline: true},
		{Name: "Racers (2)", Value: "ANN, BOB"}}})
	n.Close(5 * time.Second)
	got := f.posts["/key"]
	want := map[string]any{"title": "Race starting in lobby 3", "body": "Track: EMERALD BEACH · Laps: 3\nRacers (2): ANN, BOB",
		"group": "DreamKarts"}
	if len(got) != 1 || fmt.Sprint(got[0]) != fmt.Sprint(want) {
		t.Errorf("bark got %v, want %v", got, want)
	}
}
