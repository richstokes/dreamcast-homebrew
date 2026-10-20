package demo

import (
	"bytes"
	"compress/gzip"
	"encoding/binary"
	"testing"
)

// Every recording loads, has six karts, ends with all of them home and
// carries power-ups: someone holds an emerald in every race and throws a
// spear on every track (a quiet race on a track with few bands is fine).
func TestRecordings(t *testing.T) {
	races, err := Load()
	if err != nil {
		t.Fatal(err)
	}
	tracks, trackSpears := map[byte]int{}, map[byte]int{}
	for _, r := range races {
		tracks[r.Track]++
		if r.Karts != 6 || r.Laps != 3 {
			t.Errorf("%s: %d karts, %d laps", r.Name, r.Karts, r.Laps)
		}
		if r.RaceID == 0 {
			t.Errorf("%s: no race id (a DKR1 recording: its bots carry nothing)", r.Name)
		}
		held, spears := 0, 0
		for k := 0; k < r.Karts; k++ {
			if r.Kart(r.Frames-1, k)[0]&1 == 0 {
				t.Errorf("%s: kart %d never finishes", r.Name, k)
			}
			if r.Kart(0, k)[13] > 1 {
				t.Errorf("%s: kart %d starts on lap %d", r.Name, k, r.Kart(0, k)[13])
			}
			for f := 0; f < r.Frames; f++ {
				b := r.Kart(f, k)
				if b[KartItems]&7 != 0 {
					held++
				}
				if b[KartSeqs]&15 != 0 {
					spears++
				}
				if tg := b[KartTarget]; tg != NoTarget && int(tg) >= r.Karts {
					t.Errorf("%s: kart %d frame %d targets kart %d", r.Name, k, f, tg)
				}
			}
		}
		trackSpears[r.Track] += spears
		if held == 0 {
			t.Errorf("%s: no power-ups (no frame has a kart holding one)", r.Name)
		}
	}
	if len(tracks) < 5 {
		t.Errorf("recordings cover %d tracks", len(tracks))
	}
	for tr, n := range trackSpears {
		if n == 0 {
			t.Errorf("track %d: no spear thrown in any of its %d recordings", tr, tracks[tr])
		}
	}
}

func gz(t *testing.T, b []byte) []byte {
	var z bytes.Buffer
	w := gzip.NewWriter(&z)
	w.Write(b)
	w.Close()
	return z.Bytes()
}

// A DKR1 file (21-byte karts) loads with its item bytes zeroed; a DKR2 file
// has its protocol 2 item bits moved into today's layout and names its
// lobby and race id; a DKR3 file is taken as is.
func TestFormats(t *testing.T) {
	var b bytes.Buffer
	b.WriteString("DKR1")
	b.Write([]byte{2, 3, 1, 2})
	binary.Write(&b, binary.LittleEndian, uint32(2))
	for i := 0; i < 2*2; i++ {
		k := make([]byte, 21)
		k[0], k[13], k[20] = byte(i), 1, 0xaa
		b.Write(k)
	}
	r, err := Parse(gz(t, b.Bytes()))
	if err != nil {
		t.Fatal(err)
	}
	if r.Karts != 2 || r.Frames != 2 || r.RaceID != 0 || r.Track != 2 {
		t.Fatalf("DKR1 header %+v", r)
	}
	if k := r.Kart(1, 1); len(k) != KartLen || k[0] != 3 || k[20] != 0xaa || k[KartItems] != 0 || k[KartTarget] != 0 {
		t.Fatalf("DKR1 kart %v", k)
	}

	b.Reset()
	b.WriteString("DKR2")
	b.Write([]byte{4, 3, 2, 1})
	binary.Write(&b, binary.LittleEndian, uint32(3))
	b.Write([]byte{9, 17, 0, 0})
	for i := 0; i < 3; i++ {
		k := make([]byte, kartLen2)
		// Holding a Barrier, spun out, spear 3 thrown backwards at 625 m,
		// d -2, after kart 5.
		k[KartItems], k[20] = 0xbb, 0x44
		k[21], k[22], k[23], k[24] = 0x10, 0x27, 0xf8, 5
		b.Write(k)
	}
	r, err = Parse(gz(t, b.Bytes()))
	if err != nil {
		t.Fatal(err)
	}
	if r.Lobby != 9 || r.RaceID != 17 || r.Frames != 3 || r.Karts != 1 {
		t.Fatalf("DKR2 header %+v", r)
	}
	if k := r.Kart(2, 0); len(k) != KartLen || k[KartItems] != 0x33 || k[20] != 0x44 || k[KartSeqs] != 3 ||
		k[22] != 0x10 || k[23] != 0x27 || k[24] != 0xf8 || k[KartTarget] != 5 || k[26] != 0 || k[28] != 0 {
		t.Fatalf("DKR2 kart %v", k)
	}
	if _, err := Parse(gz(t, b.Bytes()[:len(b.Bytes())-1])); err == nil {
		t.Fatal("a short file parsed")
	}

	b.Reset()
	b.WriteString("DKR3")
	b.Write([]byte{4, 3, 2, 1})
	binary.Write(&b, binary.LittleEndian, uint32(2))
	b.Write([]byte{9, 18, 0, 0})
	for i := 0; i < 2; i++ {
		k := make([]byte, KartLen)
		k[KartItems], k[KartSeqs], k[KartTarget], k[28] = 0x04, 0x50, NoTarget, 0xf8 // a mine held, mine 5 out
		b.Write(k)
	}
	r, err = Parse(gz(t, b.Bytes()))
	if err != nil {
		t.Fatal(err)
	}
	if k := r.Kart(1, 0); r.RaceID != 18 || k[KartItems] != 4 || k[KartSeqs] != 0x50 || k[KartTarget] != NoTarget ||
		k[28] != 0xf8 {
		t.Fatalf("DKR3 kart %v", k)
	}
}
