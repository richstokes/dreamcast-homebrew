// rl-bot is a pretend Ridgeline console for testing the server and the
// game's online mode: it says hello, joins a lobby and drives a truck
// round a loop on the real mountain (heights from the level's level.bin),
// logging who else is there and where they drive. -smoke only checks the
// server answers.
package main

import (
	"encoding/binary"
	"flag"
	"fmt"
	"log"
	"math"
	"math/rand"
	"net"
	"os"
	"strings"
	"time"

	"github.com/richstokes/dreamcast-homebrew/projects/games/ridgeline/server/lobby"
)

// The level's heightfield, as src/terrain.c reads it.
const (
	levelN   = 257
	levelD   = 3.0
	levelOrg = -384.0
	cogY     = 0.86 // the Kestrel's centre of gravity above the ground
)

type terrain []float32

func loadTerrain(path string) (terrain, error) {
	b, err := os.ReadFile(path)
	if err != nil {
		return nil, err
	}
	if len(b) < levelN*levelN*8 {
		return nil, fmt.Errorf("%s: too short for a %dx%d heightfield", path, levelN, levelN)
	}
	t := make(terrain, levelN*levelN)
	for i := range t {
		t[i] = math.Float32frombits(binary.LittleEndian.Uint32(b[i*8:]))
	}
	return t, nil
}

// height and normal at (x, z), split along each cell's diagonal as drawn.
func (t terrain) at(x, z float64) (float64, [3]float64) {
	gx := math.Max(0, math.Min((x-levelOrg)/levelD, levelN-1.001))
	gz := math.Max(0, math.Min((z-levelOrg)/levelD, levelN-1.001))
	i, j := int(gx), int(gz)
	fx, fz := gx-float64(i), gz-float64(j)
	h := func(a, b int) float64 { return float64(t[(j+b)*levelN+i+a]) }
	var dx, dz float64
	if fx >= fz {
		dx, dz = h(1, 0)-h(0, 0), h(1, 1)-h(1, 0)
	} else {
		dx, dz = h(1, 1)-h(0, 1), h(0, 1)-h(0, 0)
	}
	return h(0, 0) + dx*fx + dz*fz, norm([3]float64{-dx, levelD, -dz})
}

func norm(v [3]float64) [3]float64 {
	l := math.Sqrt(v[0]*v[0] + v[1]*v[1] + v[2]*v[2])
	return [3]float64{v[0] / l, v[1] / l, v[2] / l}
}

func cross(a, b [3]float64) [3]float64 {
	return [3]float64{a[1]*b[2] - a[2]*b[1], a[2]*b[0] - a[0]*b[2], a[0]*b[1] - a[1]*b[0]}
}

func dot(a, b [3]float64) float64 { return a[0]*b[0] + a[1]*b[1] + a[2]*b[2] }

type bot struct {
	conn          *net.UDPConn
	token         uint32
	echo          uint32
	echoAt        time.Time
	name          string
	start         time.Time
	inLobby       bool
	roster        string
	seen          map[int]time.Time
	lastPos       map[int]lobby.Truck
	model, livery byte
}

func (b *bot) send(typ byte, body []byte) {
	hold := uint16(0)
	if !b.echoAt.IsZero() {
		hold = uint16(time.Since(b.echoAt) / time.Millisecond)
	}
	b.conn.Write(lobby.ClientPacket(typ, b.token, b.echo, hold, lobby.Build, body))
}

func (b *bot) hello() {
	n := make([]byte, lobby.NameLen)
	lobby.PutName(n, b.name)
	b.send(lobby.TypeHello, n)
}

// recv reads what has arrived, logging what a tester wants to know.
func (b *bot) recv(handle func(typ byte, p []byte)) {
	buf := make([]byte, 1500)
	for {
		b.conn.SetReadDeadline(time.Now().Add(time.Millisecond))
		n, err := b.conn.Read(buf)
		if err != nil {
			return
		}
		p := buf[:n]
		if n < 8 || p[0] != 'R' || p[1] != 'L' {
			continue
		}
		if p[2] != lobby.Proto {
			log.Printf("packet from a protocol %d server (this bot speaks %d)", p[2], lobby.Proto)
			continue
		}
		b.echo = binary.LittleEndian.Uint32(p[4:])
		b.echoAt = time.Now()
		handle(p[3], p)
	}
}

func (b *bot) logLobby(p []byte) {
	if len(p) < 13 {
		return
	}
	b.inLobby = true
	var names []string
	for i := 0; i < int(p[12]) && 13+(i+1)*lobby.PlayerLen <= len(p); i++ {
		e := p[13+i*lobby.PlayerLen:]
		n := fmt.Sprintf("%d:%s", e[0], lobby.CleanName(e[1:12]))
		if e[14]&lobby.FlagSummit != 0 {
			n += " (trophy)"
		}
		names = append(names, n)
	}
	r := fmt.Sprintf("lobby %d at %.2f h: %s", p[8]+1, float64(binary.LittleEndian.Uint16(p[10:]))/2000, strings.Join(names, ", "))
	if r[strings.Index(r, ":"):] != b.roster {
		b.roster = r[strings.Index(r, ":"):]
		log.Print(r)
	}
}

func (b *bot) logTrucks(p []byte) {
	if len(p) < 9 {
		return
	}
	const per = 3 + lobby.TruckLen
	for i := 0; i < int(p[8]) && 9+(i+1)*per <= len(p); i++ {
		e := p[9+i*per:]
		slot := int(e[0])
		t := lobby.DecodeTruck(e[3 : 3+lobby.TruckLen])
		if last, ok := b.seen[slot]; !ok || time.Since(last) > 5*time.Second {
			prev, had := b.lastPos[slot]
			moved := 0.0
			if had {
				moved = math.Hypot(t.Pos[0]-prev.Pos[0], t.Pos[2]-prev.Pos[2])
			}
			log.Printf("slot %d truck at %.1f, %.1f, %.1f (moved %.1f m) %.0f rpm gear %d lights %d%s", slot,
				t.Pos[0], t.Pos[1], t.Pos[2], moved, t.RPM, t.Gear, (t.Flags>>lobby.TruckLightsShift)&3,
				map[bool]string{true: " summit", false: ""}[t.Flags&lobby.TruckSummit != 0])
			b.seen[slot] = time.Now()
			b.lastPos[slot] = t
		}
	}
}

func main() {
	addr := flag.String("addr", "dcvmu.com:7536", "server address")
	name := flag.String("name", "AT-BOT", "player name (AT and a digit or dash marks a test client)")
	lobbyN := flag.Int("lobby", 9, "lobby to join, 1-10")
	smoke := flag.Bool("smoke", false, "just check the server answers: hello, the lobby list, then quit")
	dur := flag.Duration("for", 0, "quit after this long (0: run until killed)")
	summitAfter := flag.Duration("summit-after", 0, "claim the summit after this long in the lobby (0: never)")
	leaveAfter := flag.Duration("leave-after", 0, "leave the lobby after this long, then come back after -away")
	away := flag.Duration("away", 10*time.Second, "how long a -leave-after trip lasts")
	levelPath := flag.String("level", "../assets/generated/level.bin", "the level's level.bin, for ground heights")
	cx := flag.Float64("x", 0, "loop centre x (m)")
	cz := flag.Float64("z", -255, "loop centre z (m)")
	radius := flag.Float64("radius", 14, "loop radius (m)")
	speed := flag.Float64("speed", 4.5, "speed round the loop (m/s)")
	model := flag.Int("model", 0, "truck model (0 Cascade 84, 1 Kestrel, 2 Bobcat, 3 FFR)")
	livery := flag.Int("livery", 2, "livery 0-3")
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
	b := &bot{conn: conn, token: rand.Uint32() | 1, name: strings.ToUpper(*name), start: time.Now(),
		seen: map[int]time.Time{}, lastPos: map[int]lobby.Truck{}, model: byte(*model), livery: byte(*livery)}

	// Hello until welcomed.
	welcomed := false
	for tries := 0; !welcomed && tries < 24; tries++ {
		b.hello()
		deadline := time.Now().Add(500 * time.Millisecond)
		for time.Now().Before(deadline) && !welcomed {
			b.recv(func(typ byte, p []byte) {
				switch typ {
				case lobby.TypeWelcome:
					if len(p) >= 15 {
						log.Printf("welcome: latest build %d, min build %d, %d lobbies of %d", binary.LittleEndian.Uint16(p[9:]),
							binary.LittleEndian.Uint16(p[11:]), p[13], p[14])
						welcomed = true
					}
				case lobby.TypeReject:
					log.Fatalf("rejected: reason %d", p[8])
				}
			})
			time.Sleep(5 * time.Millisecond)
		}
	}
	if !welcomed {
		fmt.Println("smoke test FAILED: no answer")
		os.Exit(1)
	}
	if *smoke {
		b.send(lobby.TypeList, nil)
		deadline := time.Now().Add(2 * time.Second)
		for time.Now().Before(deadline) {
			got := false
			b.recv(func(typ byte, p []byte) {
				if typ != lobby.TypeLobbies || len(p) < 9 {
					return
				}
				players, o := 0, 9
				for i := 0; i < int(p[8]) && o+2 <= len(p); i++ {
					players += int(p[o])
					o += 2 + int(p[o])*(1+lobby.NameLen)
				}
				log.Printf("lobbies: %d players in %d lobbies", players, p[8])
				got = true
			})
			if got {
				fmt.Println("smoke test passed")
				return
			}
			time.Sleep(10 * time.Millisecond)
		}
		fmt.Println("smoke test FAILED: no lobby list")
		os.Exit(1)
	}

	ground, err := loadTerrain(*levelPath)
	if err != nil {
		log.Fatal(err)
	}
	id := *lobbyN - 1
	join := func() { b.send(lobby.TypeJoin, []byte{byte(id), b.model, b.livery}) }
	join()
	joinedAt := time.Now()
	summit := false
	away_until := time.Time{}
	left := false
	var lastSend, lastPing time.Time
	angle := 0.0
	for {
		now := time.Now()
		if *dur > 0 && now.Sub(b.start) > *dur {
			for i := 0; i < 3; i++ {
				b.send(lobby.TypeLeave, nil)
			}
			log.Print("done")
			return
		}
		b.recv(func(typ byte, p []byte) {
			switch typ {
			case lobby.TypeLobby:
				if !left {
					b.logLobby(p)
				}
			case lobby.TypeTrucks:
				b.logTrucks(p)
			case lobby.TypeReject:
				log.Printf("rejected: reason %d", p[8])
				if p[8] == lobby.RejectSession {
					b.hello()
				}
			}
		})
		if *leaveAfter > 0 && !left && away_until.IsZero() && now.Sub(joinedAt) > *leaveAfter {
			for i := 0; i < 3; i++ {
				b.send(lobby.TypeLeave, nil)
			}
			left, b.inLobby, b.roster = true, false, ""
			away_until = now.Add(*away)
			log.Print("left the lobby")
		}
		if left && now.After(away_until) {
			left = false
			joinedAt = now
			log.Print("coming back")
		}
		if left {
			if now.Sub(lastPing) > time.Second {
				b.send(lobby.TypeList, nil)
				lastPing = now
			}
			time.Sleep(5 * time.Millisecond)
			continue
		}
		if !b.inLobby && now.Sub(lastPing) > 500*time.Millisecond {
			join()
			lastPing = now
		}
		if *summitAfter > 0 && !summit && now.Sub(joinedAt) > *summitAfter {
			summit = true
			log.Print("claiming the summit")
		}
		if b.inLobby && now.Sub(lastSend) >= 83*time.Millisecond {
			dt := now.Sub(lastSend).Seconds()
			if lastSend.IsZero() || dt > 0.5 {
				dt = 0.083
			}
			lastSend = now
			angle += *speed / *radius * dt
			x, z := *cx+*radius*math.Sin(angle), *cz+*radius*math.Cos(angle)
			gy, up := ground.at(x, z)
			// Anticlockwise seen from above: forward is the circle's tangent.
			fw := [3]float64{math.Cos(angle), 0, -math.Sin(angle)}
			fw = norm([3]float64{fw[0] - up[0]*dot(fw, up), fw[1] - up[1]*dot(fw, up), fw[2] - up[2]*dot(fw, up)})
			right := cross(up, fw)
			t := lobby.Truck{
				Pos:     [3]float64{x + up[0]*cogY, gy + up[1]*cogY, z + up[2]*cogY},
				Quat:    lobby.QuatFromAxes(right, up, fw),
				Vel:     [3]float64{fw[0] * *speed, fw[1] * *speed, fw[2] * *speed},
				Steer:   -0.15,
				RPM:     1800,
				Load:    0.4,
				Gear:    2,
				ClockMs: uint32(now.Sub(b.start) / time.Millisecond),
				Flags:   1 << lobby.TruckLightsShift,
			}
			if summit {
				t.Flags |= lobby.TruckSummit
			}
			b.send(lobby.TypeTruck, t.Encode())
		}
		time.Sleep(4 * time.Millisecond)
	}
}
