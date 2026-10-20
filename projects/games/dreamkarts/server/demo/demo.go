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
// relays (flags to the spear target).
const KartLen = 25

// Offsets within a kart's bytes that the replay reads.
const (
	KartItems  = 14 // item byte: held, barrier, spun out, spear sequence
	KartTarget = 24 // the kart its spear homes on (255 none)
)

// NoTarget is a spear without a target.
const NoTarget = 255

// Headers. DKR1 recorded 21-byte karts before the item bytes existed; the
// loader pads them with zeros (no item, no spear). DKR2 adds the lobby and
// race id the recording seeded its emeralds from.
const (
	kartLen1 = 21
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
	case "DKR2":
		if len(b) < header2 {
			return nil, fmt.Errorf("short DKR2 header")
		}
		r.Lobby, r.RaceID = b[12], b[13]
		r.frames = b[header2:]
	default:
		return nil, fmt.Errorf("not a DKR recording")
	}
	if r.Karts < 1 || r.Frames < 1 || len(r.frames) != r.Frames*r.Karts*KartLen {
		return nil, fmt.Errorf("bad size: %d karts, %d frames, %d bytes", r.Karts, r.Frames, len(r.frames))
	}
	return r, nil
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
