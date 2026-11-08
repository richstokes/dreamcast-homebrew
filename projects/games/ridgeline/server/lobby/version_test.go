package lobby

import (
	"os"
	"regexp"
	"strconv"
	"testing"
)

// The console header is the single source of truth for versions.
func TestVersionMatchesHeader(t *testing.T) {
	src, err := os.ReadFile("../../src/net_version.h")
	if err != nil {
		t.Fatal(err)
	}
	want := map[string]int{"RL_NET_PROTO": Proto, "RL_NET_BUILD": Build, "RL_NET_MIN_BUILD": MinBuild}
	for name, got := range want {
		m := regexp.MustCompile(`#define\s+` + name + `\s+(\d+)`).FindSubmatch(src)
		if m == nil {
			t.Fatalf("%s missing from net_version.h", name)
		}
		v, _ := strconv.Atoi(string(m[1]))
		if v != got {
			t.Errorf("%s is %d in net_version.h but %d in lobby/proto.go", name, v, got)
		}
	}
}

// The wire sizes the console packs by hand.
func TestSizesMatchHeader(t *testing.T) {
	src, err := os.ReadFile("../../src/net.h")
	if err != nil {
		t.Fatal(err)
	}
	want := map[string]int{"NET_TRUCK_BYTES": TruckLen, "NET_PLAYER_BYTES": PlayerLen, "NET_MAX_PLAYERS": MaxPlayers,
		"NET_LOBBIES": MaxLobbies, "NET_NAME_LEN": NameLen}
	for name, got := range want {
		m := regexp.MustCompile(`#define\s+` + name + `\s+(\d+)`).FindSubmatch(src)
		if m == nil {
			t.Fatalf("%s missing from net.h", name)
		}
		v, _ := strconv.Atoi(string(m[1]))
		if v != got {
			t.Errorf("%s is %d in net.h but %d in lobby", name, v, got)
		}
	}
}
