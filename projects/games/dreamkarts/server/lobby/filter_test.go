package lobby

import (
	"context"
	"strings"
	"testing"
)

// testWords stands in for the downloaded list: made-up words, so nothing
// rude lives in this repository, enough to exercise the matching rules.
// The real list is checked by TestLiveWords.
var testWords = []string{"blorp", "zap", "gronk", "flim flam", "xx", "#comment-looking"}

func TestCensor(t *testing.T) {
	f := NewFilter(testWords)
	cases := map[string]string{
		"GG EVERYONE":         "GG EVERYONE",
		"YOU ARE A BLORPHEAD": "YOU ARE A *********",
		"what the blorp":      "what the *****",
		"B.L.O.R.P YOU":       "********* YOU",
		"bl0rp":               "*****",
		"8LORP":               "*****",
		"PUSH IT":             "PUSH IT",
		"ZAPPER":              "ZAPPER", // an entry with a longer tail is another word
		"ZAPS":                "****",
		"ZAPPED":              "ZAPPED",
		"KISS MY ZAP":         "KISS MY ***",
		"UNBLORPED":           "UNBLORPED", // an entry inside a word does not count
		"BLORPTAIL":           "BLORPTAIL",
		"YOU B L O R P":       "YOU B L O R P", // spaces keep words apart
		"BLORPBLORP":          "**********",
		"BLORPERS":            "********",
		"BULLBLORP":           "*********",
		"DUMBZAP":             "*******",
		"BULLSEYE":            "BULLSEYE",
		"":                    "",
		"***":                 "***",
		"GR0NK PLEASE":        "***** PLEASE",
		"NICE RACE GR0NK":     "NICE RACE *****",
		"A FLIM FLAM":         "A **** ****",
		"FLIM FLAMS":          "FLIM FLAMS", // phrases match exactly
		"XX":                  "XX",         // too short to be an entry
	}
	for in, want := range cases {
		if got := f.Censor(in); got != want {
			t.Errorf("Censor(%q) = %q, want %q", in, got, want)
		}
	}
	if n := f.Len(); n != len(testWords)-1 { // xx is too short; # is only a comment to ParseWords
		t.Errorf("filter has %d entries, want %d", n, len(testWords)-1)
	}
	// An empty filter changes nothing.
	for _, e := range []*Filter{nil, {}, NewFilter(nil)} {
		if got := e.Censor("what the blorp"); got != "what the blorp" {
			t.Errorf("empty filter: %q", got)
		}
	}
}

func TestParseWords(t *testing.T) {
	words, err := ParseWords(strings.NewReader("# a comment\r\nblorp\r\n\r\n  flim flam  \nzap\n"))
	if err != nil {
		t.Fatal(err)
	}
	want := []string{"blorp", "flim flam", "zap"}
	if strings.Join(words, "|") != strings.Join(want, "|") {
		t.Errorf("got %q, want %q", words, want)
	}
}

// TestLiveWords downloads the real list, as the server does, and checks
// that it stars out its own entries and leaves innocent words alone.
func TestLiveWords(t *testing.T) {
	if testing.Short() {
		t.Skip("downloads the word list")
	}
	words, from, err := FetchWords(context.Background(), DefaultWordsURL, "")
	if err != nil {
		t.Fatal(err)
	}
	f := NewFilter(words)
	if f.Len() < 300 {
		t.Fatalf("%d entries from %s, expected hundreds", f.Len(), from)
	}
	starred := 0
	for _, w := range words {
		if n, _ := normalize(w); len(n) < 3 {
			continue
		}
		if got := f.Censor(w); strings.ContainsAny(strings.ToUpper(got), "ABCDEFGHIJKLMNOPQRSTUVWXYZ") {
			t.Errorf("list entry %d came back as %q", len(w), got) // the length, not the word
		} else {
			starred++
		}
	}
	if starred < 300 {
		t.Errorf("only %d entries starred", starred)
	}
	for _, in := range []string{
		"GG EVERYONE", "HELLO FROM BOT", "ASSISTANT", "CLASS ACT", "COCKTAIL HOUR", "TITLE SCREEN",
		"GRAPE JUICE", "SCUNTHORPE UNITED", "MISSISSIPPI", "SONIC", "TAILS", "KNUCKLES", "NICE RACE",
	} {
		if got := f.Censor(in); got != in {
			t.Errorf("Censor(%q) = %q", in, got)
		}
	}
}

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
