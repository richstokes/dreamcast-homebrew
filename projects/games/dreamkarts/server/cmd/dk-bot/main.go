// dk-bot talks to a DreamKarts lobby server like a console does.
//
//	dk-bot -smoke                         check the server answers (exit 0/1)
//	dk-bot -name AT-BOT1 -lobby 0         join lobby 0 and race when a race starts
//	dk-bot -name AT-BOT1 -lobby 0 -start  also start races as soon as two are in
//	dk-bot -lobby 0 -retire 5s            drop out of each race 5 s after the green, staying in the lobby
//	dk-bot -name AT-BOT1 -lobby 0 -wave   also wave every few seconds
//	dk-bot -lobby 0 -chat "GG;HI ALL"     say each line in turn while waiting, and log the lobby's chat
//	dk-bot -lobby 9 -spectate             watch lobby 9 and log its races
//
// Keep test names starting AT- (or AT and a digit): the server sends no
// alerts for them (lobby.IsTestName).
//
// A bot's kart has no physics: it sits on the grid and reports progress at
// a steady pace so it finishes a 3-lap race in about -race seconds. Paths
// recorded with -record can be replayed with -path so it moves on track,
// interpolated to the moment of each packet, and like a console it sends
// its kart right after each RACE relay it receives (so the packet reaches
// the server well before the next relay), falling back to a timer.
package main

import (
	"bufio"
	"encoding/binary"
	"flag"
	"fmt"
	"log"
	"math"
	"math/rand"
	"net"
	"os"
	"strconv"
	"strings"
	"time"

	"github.com/richstokes/dreamcast-homebrew/projects/games/dreamkarts/server/lobby"
	"os/signal"
	"syscall"
)

var (
	addr     = flag.String("addr", "dcvmu.com:7535", "server address")
	smoke    = flag.Bool("smoke", false, "check the server answers HELLO and LIST, then exit")
	name     = flag.String("name", "AT-BOT", "player name (AT- marks a test client: no alerts)")
	lobbyID  = flag.Int("lobby", 0, "lobby to join")
	start    = flag.Bool("start", false, "start a race whenever the bot leads and two are in")
	colour   = flag.Int("colour", 7, "colour 0-7, or 8 for a custom kart (-custom)")
	custom   = flag.String("custom", "0,11,6", "custom kart palette picks: body,trim,chao (0-15)")
	raceSecs = flag.Float64("race", 90, "seconds the bot takes for the whole race")
	duration = flag.Duration("for", 0, "quit after this long (0 = forever)")
	path     = flag.String("path", "", "replay kart states from this file (lines: ms x y z heading speed lap dist)")
	wave     = flag.Bool("wave", false, "wave for 1.7 s every 5 s in the countdown and race")
	spectate = flag.Bool("spectate", false, "watch the lobby instead of racing")
	chat     = flag.String("chat", "", "lines to say in the lobby, ';' separated, one every 4 s once two are in")
	startIn  = flag.Duration("start-delay", 3*time.Second, "with -start: how long to wait in a ready lobby first")
	retire   = flag.Duration("retire", 0, "drop out of every race this long after the green (0 = race to the end)")
)

type pathPoint struct {
	ms               int
	x, y, z, hd, spd float64
	lap              int
	dist             float64
}

func main() {
	flag.Parse()
	log.SetFlags(log.Ltime | log.Lmicroseconds)
	raddr, err := net.ResolveUDPAddr("udp", *addr)
	if err != nil {
		log.Fatal(err)
	}
	conn, err := net.DialUDP("udp", nil, raddr)
	if err != nil {
		log.Fatal(err)
	}
	token := rand.Uint32() | 1
	var echo uint32
	var echoAt time.Time
	send := func(typ byte, body []byte) {
		hold := uint16(0)
		if !echoAt.IsZero() {
			hold = uint16(time.Since(echoAt) / time.Millisecond)
		}
		pkt := lobby.AppendClient(nil, lobby.ClientHeader{Type: typ, Token: token, Echo: echo, Hold: hold, Build: lobby.Build}, body)
		conn.Write(pkt)
	}
	nameBuf := make([]byte, lobby.NameLen)
	copy(nameBuf, strings.ToUpper(*name))

	if *smoke {
		os.Exit(runSmoke(conn, send, nameBuf))
	}
	var pts []pathPoint
	if *path != "" {
		pts = loadPath(*path)
	}

	buf := make([]byte, 1500)
	welcomed, joined := false, false
	racePackets := 0
	mySlot := -1
	var nonce byte
	var state, raceID, laps byte = 0, 0, 3
	var green time.Time
	var lastWish, lastHello, lastKart, readySince, lastRetire time.Time
	racing := false // our slot's racing flag in the newest LOBBY packet
	var kartDue time.Time
	var lines []string // left to say; said again from the top each time company arrives
	if *chat != "" {
		lines = strings.Split(*chat, ";")
	}
	var chatID byte
	var chatEcho bool
	var lastLine string
	var chatSince, lastChat, chatResend time.Time
	// Ctrl-C or a kill from a test harness leaves the lobby properly,
	// so the next test does not find the bot's seat still taken for the
	// 10 s session timeout.
	stop := make(chan os.Signal, 1)
	signal.Notify(stop, os.Interrupt, syscall.SIGTERM)
	began := time.Now()
	for *duration == 0 || time.Since(began) < *duration {
		select {
		case <-stop:
			log.Printf("stopping")
			send(lobby.TypeLeave, nil)
			return
		default:
		}
		conn.SetReadDeadline(time.Now().Add(20 * time.Millisecond))
		if n, err := conn.Read(buf); err == nil && n >= 8 && buf[0] == 'D' && buf[1] == 'K' {
			b := buf[:n]
			echo, echoAt = binary.LittleEndian.Uint32(b[4:]), time.Now()
			switch b[3] {
			case lobby.TypeWelcome:
				if !welcomed {
					log.Printf("welcome: status %d latest %d min %d", b[8], binary.LittleEndian.Uint16(b[9:]), binary.LittleEndian.Uint16(b[11:]))
				}
				welcomed = true
			case lobby.TypeRace:
				racePackets++
				kartDue = time.Now() // answer the relay straight away
				if *spectate && racePackets%120 == 1 && n >= 10 {
					log.Printf("race %d: %d karts", b[8], b[9])
				}
			case lobby.TypeReject:
				log.Fatalf("rejected: reason %d", b[8])
			case lobby.TypeChatMsg:
				if n >= 8+1+lobby.NameLen+2+lobby.ChatLen {
					who := lobby.CleanName(b[9 : 9+lobby.NameLen])
					if b[8] == byte(mySlot) && b[9+lobby.NameLen+1] == chatID {
						chatEcho = true
					}
					log.Printf("chat %s: %s", who, lobby.CleanChat(b[9+lobby.NameLen+2:]))
				}
			case lobby.TypeLobby:
				if !joined {
					log.Printf("in lobby %d as slot %d", b[8], b[10])
				}
				joined = true
				mySlot = int(b[10])
				ns, rid := b[9], b[15]
				racing = false
				for i := 0; i < int(b[21]) && 22+i*lobby.PlayerLen+lobby.PlayerLen <= n; i++ {
					if e := b[22+i*lobby.PlayerLen:]; int(e[0]) == mySlot && e[13]&2 != 0 {
						racing = true
					}
				}
				laps = b[13]
				t := int32(binary.LittleEndian.Uint32(b[16:]))
				if ns != state {
					log.Printf("lobby state %d -> %d (race %d, track %d, %d players)", state, ns, rid, b[12], b[21])
					if *spectate && ns == lobby.StateResults {
						logPlaces(b)
					}
				}
				if ns == lobby.StateCountdown || ns == lobby.StateRacing {
					green = time.Now().Add(time.Duration(t) * time.Millisecond)
				}
				if b[21] >= 2 && ns == lobby.StateWaiting {
					if chatSince.IsZero() {
						chatSince = time.Now()
						if *chat != "" {
							lines = strings.Split(*chat, ";")
						}
					}
				} else {
					chatSince = time.Time{}
				}
				ready := *start && ns == lobby.StateWaiting && b[11] == b[10] && b[21] >= 2
				if !ready {
					readySince = time.Time{}
				} else if readySince.IsZero() {
					readySince = time.Now()
				} else if time.Since(readySince) > *startIn {
					nonce++
					readySince = time.Now().Add(time.Hour) // once per wait
					log.Printf("starting a race")
				}
				state, raceID = ns, rid
			}
		}
		now := time.Now()
		switch {
		case !welcomed:
			if now.Sub(lastHello) > 500*time.Millisecond {
				send(lobby.TypeHello, nameBuf)
				lastHello = now
			}
		case !joined:
			if now.Sub(lastHello) > 500*time.Millisecond {
				join := []byte{byte(*lobbyID)}
				if *spectate {
					join = append(join, lobby.JoinSpectate)
				}
				send(lobby.TypeJoin, join)
				lastHello = now
			}
		case *spectate:
			if now.Sub(lastWish) > time.Second {
				send(lobby.TypePing, nil)
				lastWish = now
			}
		default:
			if now.Sub(lastWish) > 250*time.Millisecond {
				send(lobby.TypeWish, append([]byte{byte(*colour), 0, 3, 1, nonce, lobby.WishItems}, customPicks()...))
				lastWish = now
			}
			// Chat like a console: a line every 4 s once two are in, resent
			// every half second until its echo comes back (or 3 s pass).
			sayAgain := chatID != 0 && !chatEcho && now.Sub(lastChat) < 3*time.Second && now.Sub(chatResend) > 500*time.Millisecond
			sayNext := len(lines) > 0 && !chatSince.IsZero() && time.Since(chatSince) > 3*time.Second &&
				(chatID == 0 || chatEcho || now.Sub(lastChat) >= 3*time.Second) && now.Sub(lastChat) > 4*time.Second
			if sayNext {
				chatID++
				chatEcho = false
				lastLine, lines = lines[0], lines[1:]
				log.Printf("saying %q", lastLine)
				lastChat = now
				sayAgain = true
			}
			if sayAgain {
				body := make([]byte, 1+lobby.ChatLen)
				body[0] = chatID
				copy(body[1:lobby.ChatLen], lastLine)
				send(lobby.TypeChat, body)
				chatResend = now
			}
			// Retiring: like a console, ask every 250 ms until the lobby
			// state shows us out of the race.
			quit := *retire > 0 && state == lobby.StateRacing && now.Sub(green) > *retire
			if quit && racing && now.Sub(lastRetire) > 250*time.Millisecond {
				if lastRetire.IsZero() {
					log.Printf("retiring from race %d", raceID)
				}
				send(lobby.TypeRetire, []byte{raceID})
				lastRetire = now
			}
			if state == lobby.StateWaiting {
				lastRetire = time.Time{}
			}
			if (state == lobby.StateCountdown || state == lobby.StateRacing) && racing && !quit && !now.Before(kartDue) &&
				now.Sub(lastKart) > 40*time.Millisecond {
				lastKart = now
				kartDue = now.Add(100 * time.Millisecond) // unless a relay comes first
				send(lobby.TypeKart, kartBody(raceID, laps, now.Sub(green), pts))
			}
		}
	}
	send(lobby.TypeLeave, nil)
}

// customPicks parses -custom into the three palette bytes of a WISH.
func customPicks() []byte {
	picks := []byte{0, 11, 6}
	for i, f := range strings.Split(*custom, ",") {
		if i >= len(picks) {
			break
		}
		if v, err := strconv.Atoi(strings.TrimSpace(f)); err == nil && v >= 0 && v < 16 {
			picks[i] = byte(v)
		}
	}
	return picks
}

// logPlaces prints a LOBBY packet's players by their last place.
func logPlaces(b []byte) {
	n := int(b[21])
	for place := byte(1); place <= lobby.MaxPlayers; place++ {
		for i := 0; i < n && 22+i*lobby.PlayerLen+lobby.PlayerLen <= len(b); i++ {
			e := b[22+i*lobby.PlayerLen:]
			if e[18] == place {
				log.Printf("  %d. %s (%d points)", place, lobby.CleanName(e[1:12]), binary.LittleEndian.Uint16(e[16:]))
			}
		}
	}
}

// kartBody reports progress for race time t (negative before the green).
func kartBody(raceID, laps byte, t time.Duration, pts []pathPoint) []byte {
	b := make([]byte, 1+lobby.KartLen+4)
	b[0] = raceID
	frac := t.Seconds() / *raceSecs
	if frac < 0 {
		frac = 0
	}
	lap, dist := byte(1+int(frac*float64(laps))), uint16(0)
	if t < 0 {
		lap = 0
	}
	if frac >= 1 {
		b[1] |= lobby.KartFinished
		lap = laps + 1
	} else {
		dist = uint16((frac*float64(laps) - float64(int(frac*float64(laps)))) * 700 * 16)
	}
	b[1] |= 2 // grounded
	if *wave && (t+10*time.Second)%(5*time.Second) < 1700*time.Millisecond {
		b[1] |= lobby.KartWaving
	}
	if len(pts) > 0 {
		p := pathAt(pts, int(t/time.Millisecond))
		binary.LittleEndian.PutUint16(b[2:], uint16(int16(p.x*16)))
		binary.LittleEndian.PutUint16(b[4:], uint16(int16(p.y*64)))
		binary.LittleEndian.PutUint16(b[6:], uint16(int16(p.z*16)))
		binary.LittleEndian.PutUint16(b[8:], uint16(p.hd*65536/6.283185))
		binary.LittleEndian.PutUint16(b[10:], uint16(int16(p.spd*64)))
		lap, dist = byte(p.lap), uint16(p.dist*16)
		if frac < 1 {
			b[1] &^= lobby.KartFinished
		}
	}
	b[14] = lap
	binary.LittleEndian.PutUint16(b[16:], dist)
	rt := t
	if frac >= 1 {
		rt = time.Duration(*raceSecs * float64(time.Second))
	}
	if rt < 0 {
		rt = 0
	}
	binary.LittleEndian.PutUint32(b[18:], uint32(rt/time.Millisecond))
	b[26] = 255 // no spear target
	binary.LittleEndian.PutUint32(b[1+lobby.KartLen:], uint32(time.Now().UnixMilli()))
	return b
}

// pathAt is the path at ms, interpolated between the points either side;
// past the end the kart sits at the last point.
func pathAt(pts []pathPoint, ms int) pathPoint {
	if ms <= pts[0].ms {
		first := pts[0]
		first.spd = 0 // still on the grid
		return first
	}
	for i := 1; i < len(pts); i++ {
		a, b := pts[i-1], pts[i]
		if ms > b.ms {
			continue
		}
		f := float64(ms-a.ms) / float64(b.ms-a.ms)
		dh := b.hd - a.hd
		for dh > math.Pi {
			dh -= 2 * math.Pi
		}
		for dh < -math.Pi {
			dh += 2 * math.Pi
		}
		return pathPoint{ms: ms, x: a.x + (b.x-a.x)*f, y: a.y + (b.y-a.y)*f, z: a.z + (b.z-a.z)*f,
			hd: a.hd + dh*f, spd: a.spd + (b.spd-a.spd)*f, lap: a.lap, dist: a.dist + (b.dist-a.dist)*f}
	}
	last := pts[len(pts)-1]
	last.spd = 0
	return last
}

func runSmoke(conn *net.UDPConn, send func(byte, []byte), name []byte) int {
	buf := make([]byte, 1500)
	got := map[byte]bool{}
	deadline := time.Now().Add(5 * time.Second)
	for time.Now().Before(deadline) && !(got[lobby.TypeWelcome] && got[lobby.TypeLobbies]) {
		if !got[lobby.TypeWelcome] {
			send(lobby.TypeHello, name)
		} else {
			send(lobby.TypeList, nil)
		}
		conn.SetReadDeadline(time.Now().Add(300 * time.Millisecond))
		n, err := conn.Read(buf)
		if err != nil || n < 8 {
			continue
		}
		got[buf[3]] = true
		if buf[3] == lobby.TypeWelcome {
			fmt.Printf("welcome: latest build %d, min build %d, %d lobbies of %d\n",
				binary.LittleEndian.Uint16(buf[9:]), binary.LittleEndian.Uint16(buf[11:]), buf[13], buf[14])
		}
		if buf[3] == lobby.TypeLobbies {
			total := 0
			for i := 0; i < int(buf[8]); i++ {
				total += int(buf[9+i*14])
			}
			fmt.Printf("lobbies: %d players in %d lobbies\n", total, buf[8])
		}
		if buf[3] == lobby.TypeReject {
			fmt.Printf("rejected: reason %d\n", buf[8])
			return 1
		}
	}
	send(lobby.TypeLeave, nil)
	if got[lobby.TypeWelcome] && got[lobby.TypeLobbies] {
		fmt.Println("smoke test passed")
		return 0
	}
	fmt.Println("smoke test FAILED: no answer")
	return 1
}

func loadPath(file string) []pathPoint {
	f, err := os.Open(file)
	if err != nil {
		log.Fatal(err)
	}
	defer f.Close()
	var pts []pathPoint
	sc := bufio.NewScanner(f)
	for sc.Scan() {
		var p pathPoint
		if _, err := fmt.Sscan(sc.Text(), &p.ms, &p.x, &p.y, &p.z, &p.hd, &p.spd, &p.lap, &p.dist); err == nil {
			pts = append(pts, p)
		}
	}
	log.Printf("loaded %d path points", len(pts))
	return pts
}
