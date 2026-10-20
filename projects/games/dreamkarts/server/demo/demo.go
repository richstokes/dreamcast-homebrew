// Package demo holds the recorded all-CPU races that the lobby server's
// bots replay in its demo lobby. They are made on a console (or Flycast)
// by the DK_RECORD build; see ../../tools/record_demo.py.
package demo

import (
	"bytes"
	"compress/gzip"
	"embed"
	"encoding/binary"
	"fmt"
	"io"
	"path"
	"sort"
)

// Rate is the recorded frames per second, from the green light.
const Rate = 12

// KartLen is one kart's recorded state: the KART body bytes the server
// relays (flags to the mine).
const KartLen = 29

// Offsets within a kart's bytes that the replay reads.
const (
	KartItems  = 14 // item byte: held, barrier, spun out, spear thrown backwards
	KartSeqs   = 21 // sequence byte: spear in the low nibble, mine in the high
	KartTarget = 25 // the kart its spear homes on (255 none)
)

// NoTarget is a spear without a target.
const NoTarget = 255

// Headers. DKR1 recorded 21-byte karts before the item bytes existed; the
// loader pads them with zeros (no item, no spear). DKR2 adds the lobby and
// race id the recording seeded its emeralds from, with the 25-byte kart of
// protocol 2 (no mine), which the loader rewrites into today's layout.
// DKR3 holds the 29-byte kart of protocol 3.
const (
	kartLen1 = 21
	kartLen2 = 25
	header1  = 12
	header2  = 16
)

//go:embed races/*.dkr.gz
var files embed.FS

// Race is one recorded race.
type Race struct {
	Name            string
	Track, Laps, CC byte
	Karts           int
	// Lobby and RaceID seeded the recording's emeralds (0 = unknown, a
	// DKR1 file): replayed as that race id in that lobby, watchers see the
	// gems where the bots took them.
	Lobby, RaceID byte
	frames        []byte
	Frames        int
}

// Kart returns kart k's state at frame f (clamped to the recording).
func (r *Race) Kart(f, k int) []byte {
	if f < 0 {
		f = 0
	}
	if f >= r.Frames {
		f = r.Frames - 1
	}
	o := (f*r.Karts + k) * KartLen
	return r.frames[o : o+KartLen]
}

// Load reads every embedded race.
func Load() ([]*Race, error) {
	names, err := files.ReadDir("races")
	if err != nil {
		return nil, err
	}
	var races []*Race
	for _, e := range names {
		b, err := files.ReadFile(path.Join("races", e.Name()))
		if err != nil {
			return nil, err
		}
		r, err := Parse(b)
		if err != nil {
			return nil, fmt.Errorf("%s: %w", e.Name(), err)
		}
		r.Name = e.Name()
		races = append(races, r)
	}
	sort.Slice(races, func(i, j int) bool { return races[i].Name < races[j].Name })
	return races, nil
}

// Parse reads one gzipped recording.
func Parse(gz []byte) (*Race, error) {
	zr, err := gzip.NewReader(bytes.NewReader(gz))
	if err != nil {
		return nil, err
	}
	b, err := io.ReadAll(zr)
	if err != nil {
		return nil, err
	}
	if len(b) < header1 {
		return nil, fmt.Errorf("not a DKR recording")
	}
	r := &Race{Track: b[4], Laps: b[5], CC: b[6], Karts: int(b[7]), Frames: int(binary.LittleEndian.Uint32(b[8:]))}
	switch string(b[:4]) {
	case "DKR1":
		r.frames = pad(b[header1:], kartLen1)
	case "DKR2", "DKR3":
		if len(b) < header2 {
			return nil, fmt.Errorf("short %s header", b[:4])
		}
		r.Lobby, r.RaceID = b[12], b[13]
		r.frames = b[header2:]
		if string(b[:4]) == "DKR2" {
			r.frames = upgrade2(r.frames)
		}
	default:
		return nil, fmt.Errorf("not a DKR recording")
	}
	if r.Karts < 1 || r.Frames < 1 || len(r.frames) != r.Frames*r.Karts*KartLen {
		return nil, fmt.Errorf("bad size: %d karts, %d frames, %d bytes", r.Karts, r.Frames, len(r.frames))
	}
	return r, nil
}

// upgrade2 rewrites protocol 2 karts (25 bytes) into the KartLen layout:
// the held item grows to three bits, the barrier, spin-out and backwards
// bits move up, the spear sequence gets its own byte and the spear bytes
// follow it; no mine.
func upgrade2(b []byte) []byte {
	if len(b)%kartLen2 != 0 {
		return b
	}
	out := make([]byte, 0, len(b)/kartLen2*KartLen)
	for i := 0; i < len(b); i += kartLen2 {
		k := b[i : i+kartLen2]
		n := make([]byte, KartLen)
		copy(n, k[:KartItems])
		old := k[KartItems]
		n[KartItems] = (old & 3) | ((old >> 2) & 1 << 3) | ((old >> 3) & 1 << 4) | ((old >> 7) & 1 << 5)
		copy(n[KartItems+1:KartSeqs], k[KartItems+1:KartSeqs])
		n[KartSeqs] = (old >> 4) & 7
		copy(n[KartSeqs+1:], k[KartSeqs:])
		out = append(out, n...)
	}
	return out
}

// pad widens karts of n bytes to KartLen with zeros.
func pad(b []byte, n int) []byte {
	if len(b)%n != 0 {
		return b
	}
	out := make([]byte, 0, len(b)/n*KartLen)
	for i := 0; i < len(b); i += n {
		out = append(out, b[i:i+n]...)
		out = append(out, make([]byte, KartLen-n)...)
	}
	return out
}
