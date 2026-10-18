// dreamkarts-lobby is the DreamKarts online lobby server: ten lobbies of up
// to six Dreamcasts over UDP. See ../../PROTOCOL.md and ../../README.md.
package main

import (
	"flag"
	"log"
	"net"
	"net/netip"
	"os"
	"os/signal"
	"strconv"
	"syscall"
	"time"

	"github.com/richstokes/dreamcast-homebrew/projects/games/dreamkarts/server/demo"
	"github.com/richstokes/dreamcast-homebrew/projects/games/dreamkarts/server/lobby"
)

type packet struct {
	from netip.AddrPort
	data []byte
}

func main() {
	listen := flag.String("listen", envOr("DREAMKARTS_LISTEN", ":7535"), "UDP address to listen on")
	latest := flag.Int("latest", lobby.Build, "console build to call the latest (default: src/net_version.h)")
	minBuild := flag.Int("min", lobby.MinBuild, "oldest console build allowed to play")
	demoLobby := flag.Int("demo-lobby", envInt("DREAMKARTS_DEMO_LOBBY", 10), "lobby (1-10) where bots race forever, 0 for none")
	flag.Parse()
	log.SetFlags(0) // journald adds timestamps

	addr, err := net.ResolveUDPAddr("udp", *listen)
	if err != nil {
		log.Fatal(err)
	}
	conn, err := net.ListenUDP("udp", addr)
	if err != nil {
		log.Fatal(err)
	}
	log.Printf("dreamkarts-lobby protocol %d build %d (min %d) on %v", lobby.Proto, lobby.Build, lobby.MinBuild, conn.LocalAddr())

	in := make(chan packet, 1024)
	go func() {
		buf := make([]byte, 1500)
		for {
			n, from, err := conn.ReadFromUDPAddrPort(buf)
			if err != nil {
				if ne, ok := err.(net.Error); ok && ne.Timeout() {
					continue
				}
				log.Printf("read: %v", err)
				close(in)
				return
			}
			if n > 512 {
				continue
			}
			select {
			case in <- packet{from, append([]byte(nil), buf[:n]...)}:
			default: // overloaded: drop rather than queue stale state
			}
		}
	}()

	srv := lobby.New(func(to netip.AddrPort, b []byte) {
		if _, err := conn.WriteToUDPAddrPort(b, to); err != nil {
			log.Printf("write %v: %v", to, err)
		}
	}, time.Now())
	srv.Latest, srv.Min = uint16(*latest), uint16(*minBuild)
	if *latest != lobby.Build || *minBuild != lobby.MinBuild {
		log.Printf("overriding versions: latest %d, min %d", *latest, *minBuild)
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
	for {
		select {
		case p, ok := <-in:
			if !ok {
				return
			}
			srv.Handle(p.from, p.data, time.Now())
		case now := <-tick.C:
			srv.Tick(now)
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
