// ridgeline-lobby is the Ridgeline online server: ten lobbies of up to
// four Dreamcasts on the mountain, over UDP. See ../../PROTOCOL.md and
// ../../README.md.
package main

import (
	"flag"
	"log"
	"os"
	"os/signal"
	"syscall"
	"time"

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
	lag := flag.Duration("lag", 0, "testing: pretend every console is this far away (round trip)")
	jitter := flag.Duration("jitter", 0, "testing: plus up to this much extra on every packet, each way")
	flag.Parse()
	log.SetFlags(0) // journald adds timestamps

	conn, err := udpio.Listen(*listen, *lag, *jitter)
	if err != nil {
		log.Fatal(err)
	}
	log.Printf("ridgeline-lobby protocol %d build %d (min %d) on %v", lobby.Proto, lobby.Build, lobby.MinBuild, conn.LocalAddr())
	srv := lobby.New(conn.Send, time.Now())
	srv.Latest, srv.Min = uint16(*latest), uint16(*minBuild)
	if *latest != lobby.Build || *minBuild != lobby.MinBuild {
		log.Printf("overriding versions: latest %d, min %d", *latest, *minBuild)
	}
	// Names only: Ridgeline has no chat. Consoles that arrive while the
	// list downloads wait for their hello to be answered.
	srv.Filter = wordfilter.Load(*wordsURL, *wordsExtra, *wordsCache, log.Printf)

	stop := make(chan os.Signal, 1)
	signal.Notify(stop, syscall.SIGINT, syscall.SIGTERM)
	tick := time.NewTicker(20 * time.Millisecond)
	defer tick.Stop()
	relay := time.NewTicker(lobby.RelayInterval)
	defer relay.Stop()
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
