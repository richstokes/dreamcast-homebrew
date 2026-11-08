package lobby

import "testing"

// testWords stands in for the downloaded bad word list in the server's
// tests: made-up words, so nothing rude lives in this repository.
var testWords = []string{"blorp", "zap", "gronk", "flim flam", "xx", "#comment-looking"}

func TestCleanChat(t *testing.T) {
	cases := map[string]string{
		"hello there":         "HELLO THERE",
		"  two   spaces  ":    "TWO SPACES",
		"tab\tand\x01control": "TABANDCONTROL",
		"":                    "",
		"   ":                 "",
	}
	for in, want := range cases {
		if got := CleanChat([]byte(in)); got != want {
			t.Errorf("CleanChat(%q) = %q, want %q", in, got, want)
		}
	}
	long := CleanChat([]byte("ABCDEFGHIJKLMNOPQRSTUVWXYZ ABCDEFGHIJKLMNOPQRSTUVWXYZ"))
	if len(long) != ChatLen-1 {
		t.Errorf("long line kept %d characters, want %d", len(long), ChatLen-1)
	}
	if trailing := CleanChat([]byte("ABCDEFGHIJKLMNOPQRSTUVWXYZ12345 6789")); trailing[len(trailing)-1] == ' ' {
		t.Errorf("a cut line ends in a space: %q", trailing)
	}
}
