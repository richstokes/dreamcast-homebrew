// ridgeline-lobby is the Ridgeline online server: twelve lobbies of up to
// four Dreamcasts on the mountain, over UDP. See ../../PROTOCOL.md and
// ../../README.md.
package main

import (
	"flag"
	"fmt"
	"log"
	"os"
	"os/signal"
	"strings"
	"syscall"
	"time"

	"github.com/richstokes/dreamcast-homebrew/projects/common/gameserver/alerts"
	"github.com/richstokes/dreamcast-homebrew/projects/common/gameserver/status"
	"github.com/richstokes/dreamcast-homebrew/projects/common/gameserver/udpio"
	"github.com/richstokes/dreamcast-homebrew/projects/common/gameserver/wordfilter"
	"github.com/richstokes/dreamcast-homebrew/projects/games/ridgeline/server/lobby"
)

func main() {
	listen := flag.String("listen", envOr("RIDGELINE_LISTEN", ":7536"), "UDP address to listen on")
	latest := flag.Int("latest", lobby.Build, "console build to call the latest (default: src/net_version.h)")
	minBuild := flag.Int("min", lobby.MinBuild, "oldest console build allowed to play")
	wordsURL := flag.String("words-url", envOr("RIDGELINE_BAD_WORDS_URL", wordfilter.DefaultWordsURL), "bad word list to download at start, one word per line")
	wordsExtra := flag.String("words-extra", os.Getenv("RIDGELINE_BAD_WORDS_EXTRA"), "comma-separated words to filter on top of the list")
	wordsCache := flag.String("words-cache", wordfilter.DefaultCache(), "file keeping the last downloaded list, read when the download fails (empty: none)")
	alertsConfig := flag.String("alerts", os.Getenv("RIDGELINE_ALERTS_CONFIG"), "JSON file of Discord webhooks and Bark keys for lobby alerts (empty or missing: none)")
	statusAddr := flag.String("status", envOr("RIDGELINE_STATUS", "127.0.0.1:8094"), "loopback HTTP address for lobbies.json (dcvmu.com/lobbies; empty: off)")
	lag := flag.Duration("lag", 0, "testing: pretend every console is this far away (round trip)")
	jitter := flag.Duration("jitter", 0, "testing: plus up to this much extra on every packet, each way")
	idle := flag.Duration("idle", lobby.IdleTimeout, "take a truck out of its lobby after this long idle")
	flag.Parse()
	log.SetFlags(0) // journald adds timestamps

	conn, err := udpio.Listen(*listen, *lag, *jitter)
	if err != nil {
		log.Fatal(err)
	}
	log.Printf("ridgeline-lobby protocol %d build %d (min %d) on %v", lobby.Proto, lobby.Build, lobby.MinBuild, conn.LocalAddr())
	srv := lobby.New(conn.Send, time.Now())
	srv.Idle = *idle
	srv.Latest, srv.Min = uint16(*latest), uint16(*minBuild)
	if *latest != lobby.Build || *minBuild != lobby.MinBuild {
		log.Printf("overriding versions: latest %d, min %d", *latest, *minBuild)
	}
	// Names only: Ridgeline has no chat. Consoles that arrive while the
	// list downloads wait for their hello to be answered.
	srv.Filter = wordfilter.Load(*wordsURL, *wordsExtra, *wordsCache, log.Printf)

	// No alerts beats no lobby: a bad alerts file is logged, not fatal.
	notifier, err := alerts.Load(*alertsConfig, alerts.Game{Name: "Ridgeline", Icon: icon}, log.Printf)
	if err != nil {
		log.Printf("alerts off: %v", err)
	}
	log.Printf("alerts: %s", notifier.Names())
	defer notifier.Close(5 * time.Second)
	srv.OnJoin = func(j lobby.Join, now time.Time) {
		notifier.Notify(alerts.LobbyJoin, alerts.Message{
			Title:   fmt.Sprintf("%s joined lobby %d", j.Name, j.Lobby),
			Color:   0x2ecc71,
			Fields:  []alerts.Field{{Name: fmt.Sprintf("On the mountain (%d/%d)", len(j.Players), lobby.MaxPlayers), Value: strings.Join(j.Players, ", ")}},
			Time:    now,
			Key:     j.Name, // one alert per player per throttle window (5 min by default)
			Players: []string{j.Name},
		})
	}

	stop := make(chan os.Signal, 1)
	signal.Notify(stop, syscall.SIGINT, syscall.SIGTERM)
	tick := time.NewTicker(20 * time.Millisecond)
	defer tick.Stop()
	relay := time.NewTicker(lobby.RelayInterval)
	defer relay.Stop()
	var pub status.Publisher
	pub.Publish(srv.Snapshot(time.Now()))
	pub.Listen(*statusAddr)
	publish := time.NewTicker(status.Every)
	defer publish.Stop()
	for {
		select {
		case p, ok := <-conn.In():
			if !ok {
				return
			}
			srv.Handle(p.From, p.Data, time.Now())
		case <-relay.C:
			srv.Relay(time.Now())
		case <-tick.C:
			srv.Tick(time.Now())
		case <-publish.C:
			pub.Publish(srv.Snapshot(time.Now()))
		case <-stop:
			log.Printf("shutting down with %d sessions", srv.Players())
			return
		}
	}
}

// icon is the game icon Bark shows beside an alert.
const icon = "https://raw.githubusercontent.com/richstokes/dreamcast-homebrew/main/projects/games/ridgeline/assets/icon.png"

func envOr(k, def string) string {
	if v := os.Getenv(k); v != "" {
		return v
	}
	return def
}
