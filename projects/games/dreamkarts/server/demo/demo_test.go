package demo

import "testing"

// Every recording loads, has six karts, and ends with all of them home.
func TestRecordings(t *testing.T) {
	races, err := Load()
	if err != nil {
		t.Fatal(err)
	}
	tracks := map[byte]int{}
	for _, r := range races {
		tracks[r.Track]++
		if r.Karts != 6 || r.Laps != 3 {
			t.Errorf("%s: %d karts, %d laps", r.Name, r.Karts, r.Laps)
		}
		for k := 0; k < r.Karts; k++ {
			if r.Kart(r.Frames-1, k)[0]&1 == 0 {
				t.Errorf("%s: kart %d never finishes", r.Name, k)
			}
			if r.Kart(0, k)[13] > 1 {
				t.Errorf("%s: kart %d starts on lap %d", r.Name, k, r.Kart(0, k)[13])
			}
		}
	}
	if len(tracks) < 5 {
		t.Errorf("recordings cover %d tracks", len(tracks))
	}
}
