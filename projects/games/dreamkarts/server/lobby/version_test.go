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
	want := map[string]int{"DK_NET_PROTO": Proto, "DK_NET_BUILD": Build, "DK_NET_MIN_BUILD": MinBuild}
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
