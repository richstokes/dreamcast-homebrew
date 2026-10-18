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
// relays (flags to race time).
const KartLen = 21

//go:embed races/*.dkr.gz
var files embed.FS

// Race is one recorded race.
type Race struct {
	Name            string
	Track, Laps, CC byte
	Karts           int
	frames          []byte
	Frames          int
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
	if len(b) < 12 || string(b[:4]) != "DKR1" {
		return nil, fmt.Errorf("not a DKR1 recording")
	}
	r := &Race{Track: b[4], Laps: b[5], CC: b[6], Karts: int(b[7]), Frames: int(binary.LittleEndian.Uint32(b[8:]))}
	r.frames = b[12:]
	if r.Karts < 1 || r.Frames < 1 || len(r.frames) != r.Frames*r.Karts*KartLen {
		return nil, fmt.Errorf("bad size: %d karts, %d frames, %d bytes", r.Karts, r.Frames, len(r.frames))
	}
	return r, nil
}
