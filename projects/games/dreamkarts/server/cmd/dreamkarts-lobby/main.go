// dreamkarts-lobby is the DreamKarts online lobby server: ten lobbies of up
// to six Dreamcasts over UDP. See ../../PROTOCOL.md and ../../README.md.
package main

import (
	"flag"
	"log"
	"os"
	"os/signal"
	"strconv"
	"syscall"
	"time"

	"github.com/richstokes/dreamcast-homebrew/projects/common/gameserver/udpio"
	"github.com/richstokes/dreamcast-homebrew/projects/common/gameserver/wordfilter"
	"github.com/richstokes/dreamcast-homebrew/projects/games/dreamkarts/server/alerts"
	"github.com/richstokes/dreamcast-homebrew/projects/games/dreamkarts/server/demo"
	"github.com/richstokes/dreamcast-homebrew/projects/games/dreamkarts/server/lobby"
)

func main() {
	listen := flag.String("listen", envOr("DREAMKARTS_LISTEN", ":7535"), "UDP address to listen on")
	latest := flag.Int("latest", lobby.Build, "console build to call the latest (default: src/net_version.h)")
	minBuild := flag.Int("min", lobby.MinBuild, "oldest console build allowed to play")
	demoLobby := flag.Int("demo-lobby", envInt("DREAMKARTS_DEMO_LOBBY", 10), "lobby (1-10) where bots race forever, 0 for none")
	wordsURL := flag.String("words-url", envOr("DREAMKARTS_BAD_WORDS_URL", wordfilter.DefaultWordsURL), "bad word list to download at start, one word per line")
	wordsExtra := flag.String("words-extra", os.Getenv("DREAMKARTS_BAD_WORDS_EXTRA"), "comma-separated words to filter on top of the list")
	alertsConfig := flag.String("alerts", os.Getenv("DREAMKARTS_ALERTS_CONFIG"), "JSON file of Discord webhooks and Bark keys for lobby alerts (empty or missing: none)")
	wordsCache := flag.String("words-cache", wordfilter.DefaultCache(), "file keeping the last downloaded list, read when the download fails (empty: none)")
	lag := flag.Duration("lag", 0, "testing: pretend every console is this far away (round trip)")
	jitter := flag.Duration("jitter", 0, "testing: plus up to this much extra on every packet, each way")
	rejectProto := flag.Int("reject-proto", 0, "testing: answer every HELLO with a REJECT stamped with this protocol byte, like a server that moved on")
	raceSeed := flag.Int("race-seed", -1, "testing: every lobby's first race is this plus one (default random), so a restart repeats race ids")
	flag.Parse()
	log.SetFlags(0) // journald adds timestamps

	conn, err := udpio.Listen(*listen, *lag, *jitter)
	if err != nil {
		log.Fatal(err)
	}
	log.Printf("dreamkarts-lobby protocol %d build %d (min %d) on %v", lobby.Proto, lobby.Build, lobby.MinBuild, conn.LocalAddr())

	srv := lobby.New(conn.Send, time.Now())
	srv.Latest, srv.Min = uint16(*latest), uint16(*minBuild)
	if *rejectProto != 0 {
		srv.RejectProto = byte(*rejectProto)
		log.Printf("testing: rejecting every hello as protocol %d", *rejectProto)
	}
	if *raceSeed >= 0 {
		srv.SeedRaces(byte(*raceSeed))
		log.Printf("testing: race ids start at %d", *raceSeed+1)
	}
	srv.Filter = wordfilter.Load(*wordsURL, *wordsExtra, *wordsCache, log.Printf)
	if *latest != lobby.Build || *minBuild != lobby.MinBuild {
		log.Printf("overriding versions: latest %d, min %d", *latest, *minBuild)
	}

	// No alerts beats no lobby: a bad alerts file is logged, not fatal.
	notifier, err := alerts.Load(*alertsConfig, log.Printf)
	if err != nil {
		log.Printf("alerts off: %v", err)
	}
	log.Printf("alerts: %s", notifier.Names())
	defer notifier.Close(5 * time.Second)
	srv.OnEvent = func(e lobby.Event, now time.Time) {
		notifier.Notify(alert(e, now))
	}

	if *demoLobby > 0 {
		races, err := demo.Load()
		if err != nil {
			log.Fatalf("demo races: %v", err)
		}
		srv.EnableDemo(*demoLobby-1, races, time.Now().UnixNano(), time.Now())
	}

	stop := make(chan os.Signal, 1)
	signal.Notify(stop, syscall.SIGINT, syscall.SIGTERM)
	tick := time.NewTicker(20 * time.Millisecond)
	defer tick.Stop()
	relay := time.NewTicker(lobby.RaceInterval) // karts go out on the beat
	defer relay.Stop()
	for {
		select {
		case p, ok := <-conn.In():
			if !ok {
				return
			}
			srv.Handle(p.From, p.Data, time.Now())
		case <-relay.C:
			srv.Relay(time.Now()) // not the tick's time: packets handled since carry time.Now()
		case <-tick.C:
			srv.Tick(time.Now())
		case <-stop:
			log.Printf("shutting down with %d sessions", srv.Players())
			return
		}
	}
}

func envOr(k, def string) string {
	if v := os.Getenv(k); v != "" {
		return v
	}
	return def
}

func envInt(k string, def int) int {
	if v, err := strconv.Atoi(os.Getenv(k)); err == nil {
		return v
	}
	return def
}
