// Package discord posts lobby alerts to Discord webhooks. The webhooks live
// in a JSON file outside this repository (a webhook URL is its own
// password), installed on the host by richstokes/infra:
//
//	{"webhooks": [{"name": "dcnet", "url": "https://discord.com/api/webhooks/...",
//	               "events": ["lobby_join", "race_start"],
//	               "throttle": {"lobby_join": "5m"}}]}
//
// Each webhook picks its events and gets its own throttle per event: an
// alert inside the throttle window since that webhook's last alert of the
// same event is dropped, not queued. Posting happens on one goroutine per
// webhook so a slow Discord never holds up the game; a full queue drops.
// URLs are never logged, only names.
package discord

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net/http"
	"net/url"
	"os"
	"strconv"
	"strings"
	"time"
)

// Event names, as used in the config file.
type Event string

const (
	LobbyJoin Event = "lobby_join" // a player took a slot in a lobby
	RaceStart Event = "race_start" // a race with company is counting down
)

// DefaultThrottle is the least time between two alerts of an event on one
// webhook, unless its config says otherwise. Joins are common, so at most
// one every five minutes; a race starting is news every time.
var DefaultThrottle = map[Event]time.Duration{
	LobbyJoin: 5 * time.Minute,
	RaceStart: 0,
}

// Embed is a Discord message embed.
type Embed struct {
	Title       string  `json:"title,omitempty"`
	Description string  `json:"description,omitempty"`
	Color       int     `json:"color,omitempty"`
	Fields      []Field `json:"fields,omitempty"`
	Footer      *Footer `json:"footer,omitempty"`
	Timestamp   string  `json:"timestamp,omitempty"`
}

type Field struct {
	Name   string `json:"name"`
	Value  string `json:"value"`
	Inline bool   `json:"inline,omitempty"`
}

type Footer struct {
	Text string `json:"text"`
}

// Config is the webhook file.
type Config struct {
	Webhooks []Webhook `json:"webhooks"`
}

type Webhook struct {
	Name     string             `json:"name"`
	URL      string             `json:"url"`
	Events   []Event            `json:"events"`
	Throttle map[Event]Duration `json:"throttle,omitempty"`
	Username string             `json:"username,omitempty"` // default "DreamKarts"
}

// Duration reads "5m" style durations from JSON.
type Duration time.Duration

func (d *Duration) UnmarshalJSON(b []byte) error {
	var s string
	if err := json.Unmarshal(b, &s); err != nil {
		return fmt.Errorf("throttle must be a duration string like \"5m\": %w", err)
	}
	v, err := time.ParseDuration(s)
	if err != nil {
		return err
	}
	*d = Duration(v)
	return nil
}

// Parse checks a config: every webhook needs a unique name, an https URL
// (plain http only to this machine, for testing) and only known events.
func Parse(b []byte) (*Config, error) {
	var c Config
	dec := json.NewDecoder(bytes.NewReader(b))
	dec.DisallowUnknownFields()
	if err := dec.Decode(&c); err != nil {
		return nil, err
	}
	names := map[string]bool{}
	for i, w := range c.Webhooks {
		if w.Name == "" {
			return nil, fmt.Errorf("webhook %d has no name", i+1)
		}
		if names[w.Name] {
			return nil, fmt.Errorf("two webhooks are called %q", w.Name)
		}
		names[w.Name] = true
		u, err := url.Parse(w.URL)
		local := err == nil && u.Scheme == "http" && (u.Hostname() == "127.0.0.1" || u.Hostname() == "localhost")
		if err != nil || (u.Scheme != "https" && !local) || u.Host == "" {
			return nil, fmt.Errorf("webhook %q: url must be an https webhook URL", w.Name)
		}
		for _, ev := range w.Events {
			if _, ok := DefaultThrottle[ev]; !ok {
				return nil, fmt.Errorf("webhook %q: unknown event %q", w.Name, ev)
			}
		}
		for ev := range w.Throttle {
			if _, ok := DefaultThrottle[ev]; !ok {
				return nil, fmt.Errorf("webhook %q: throttle for unknown event %q", w.Name, ev)
			}
		}
	}
	return &c, nil
}

// Notifier sends alerts to every webhook that wants them. A nil Notifier
// sends nothing. Notify must be called from one goroutine (the server's).
type Notifier struct {
	hooks []*hook
	logf  func(string, ...any)
}

type hook struct {
	cfg      Webhook
	throttle map[Event]time.Duration
	last     map[Event]time.Time
	queue    chan []byte
	done     chan struct{}
	stop     context.Context
	cancel   context.CancelFunc
}

// QueueLen is how many alerts one webhook holds while Discord is slow.
const QueueLen = 16

// Client posts the alerts; tests swap it.
var Client = &http.Client{Timeout: 10 * time.Second}

// Load reads the webhook file at path. An empty path or a missing file
// means no alerts and no error.
func Load(path string, logf func(string, ...any)) (*Notifier, error) {
	if path == "" {
		return nil, nil
	}
	b, err := os.ReadFile(path)
	if errors.Is(err, os.ErrNotExist) {
		return nil, nil
	}
	if err != nil {
		return nil, err
	}
	c, err := Parse(b)
	if err != nil {
		return nil, fmt.Errorf("%s: %w", path, err)
	}
	return New(c, logf), nil
}

// New starts a sender for each webhook in c.
func New(c *Config, logf func(string, ...any)) *Notifier {
	n := &Notifier{logf: logf}
	for _, w := range c.Webhooks {
		h := &hook{cfg: w, throttle: map[Event]time.Duration{}, last: map[Event]time.Time{},
			queue: make(chan []byte, QueueLen), done: make(chan struct{})}
		h.stop, h.cancel = context.WithCancel(context.Background())
		for _, ev := range w.Events {
			h.throttle[ev] = DefaultThrottle[ev]
			if d, ok := w.Throttle[ev]; ok {
				h.throttle[ev] = time.Duration(d)
			}
		}
		if h.cfg.Username == "" {
			h.cfg.Username = "DreamKarts"
		}
		n.hooks = append(n.hooks, h)
		go n.run(h)
	}
	return n
}

// Names lists the webhooks and their events, for the startup log.
func (n *Notifier) Names() string {
	if n == nil {
		return "none"
	}
	var parts []string
	for _, h := range n.hooks {
		var evs []string
		for _, ev := range h.cfg.Events {
			evs = append(evs, string(ev))
		}
		parts = append(parts, fmt.Sprintf("%s (%s)", h.cfg.Name, strings.Join(evs, ", ")))
	}
	return strings.Join(parts, "; ")
}

// Notify queues e for every webhook that takes ev and is outside its
// throttle window.
func (n *Notifier) Notify(ev Event, e Embed, now time.Time) {
	if n == nil {
		return
	}
	for _, h := range n.hooks {
		d, ok := h.throttle[ev]
		if !ok {
			continue
		}
		if last, sent := h.last[ev]; sent && now.Sub(last) < d {
			continue
		}
		h.last[ev] = now
		body, err := json.Marshal(map[string]any{
			"username":         h.cfg.Username,
			"embeds":           []Embed{e},
			"allowed_mentions": map[string]any{"parse": []string{}}, // no player name may ping anyone
		})
		if err != nil {
			n.logf("discord %s: %v", h.cfg.Name, err)
			continue
		}
		select {
		case h.queue <- body:
		default:
			n.logf("discord %s: queue full, dropped a %s alert", h.cfg.Name, ev)
		}
	}
}

// Close sends what is queued, giving up after wait.
func (n *Notifier) Close(wait time.Duration) {
	if n == nil {
		return
	}
	for _, h := range n.hooks {
		close(h.queue)
	}
	deadline := time.After(wait)
	for _, h := range n.hooks {
		select {
		case <-h.done:
		case <-deadline:
			for _, h := range n.hooks {
				h.cancel()
			}
			return
		}
	}
}

func (n *Notifier) run(h *hook) {
	defer close(h.done)
	for body := range h.queue {
		n.post(h, body)
	}
}

// post sends one alert, waiting out Discord's rate limit a few times.
func (n *Notifier) post(h *hook, body []byte) {
	for attempt := 1; attempt <= 3; attempt++ {
		req, err := http.NewRequestWithContext(h.stop, http.MethodPost, h.cfg.URL, bytes.NewReader(body))
		if err != nil {
			n.logf("discord %s: bad request", h.cfg.Name)
			return
		}
		req.Header.Set("Content-Type", "application/json")
		resp, err := Client.Do(req)
		if err != nil {
			var ue *url.Error
			if errors.As(err, &ue) {
				err = ue.Err // the url.Error text carries the URL, and with it the token
			}
			n.logf("discord %s: %v", h.cfg.Name, err)
			return
		}
		msg, _ := io.ReadAll(io.LimitReader(resp.Body, 512))
		resp.Body.Close()
		switch {
		case resp.StatusCode < 300:
			return
		case resp.StatusCode == http.StatusTooManyRequests && attempt < 3:
			wait := retryAfter(resp.Header, msg)
			n.logf("discord %s: rate limited, retrying in %v", h.cfg.Name, wait)
			select {
			case <-time.After(wait):
			case <-h.stop.Done():
				return
			}
		default:
			n.logf("discord %s: HTTP %d: %s", h.cfg.Name, resp.StatusCode, strings.TrimSpace(string(msg)))
			return
		}
	}
}

// retryAfter reads how long Discord wants us to wait: the JSON body's
// retry_after (seconds, fractional), else the Retry-After header, capped
// so a bad answer cannot park the sender for long.
func retryAfter(hdr http.Header, body []byte) time.Duration {
	secs := 1.0
	var b struct {
		RetryAfter float64 `json:"retry_after"`
	}
	if json.Unmarshal(body, &b) == nil && b.RetryAfter > 0 {
		secs = b.RetryAfter
	} else if v, err := strconv.ParseFloat(hdr.Get("Retry-After"), 64); err == nil && v > 0 {
		secs = v
	}
	if secs > 60 {
		secs = 60
	}
	return time.Duration(secs * float64(time.Second))
}
