package lobby

import (
	"context"
	"strings"
	"testing"
)

// testWords stands in for the downloaded list: enough entries to exercise
// the matching rules. The real list is checked by TestLiveWords.
var testWords = []string{
	"fuck", "shit", "cunt", "bitch", "nigga", "ass", "cock", "tit", "cum", "rape", "spic", "homo",
	"arsehole", "blow job", "piece of shit", "xx", "#comment-looking", "Scunthorpe",
}

func TestCensor(t *testing.T) {
	f := NewFilter(testWords)
	cases := map[string]string{
		"GG EVERYONE":        "GG EVERYONE",
		"YOU ARE A SHITHEAD": "YOU ARE A ********",
		"what the fuck":      "what the ****",
		"F.U.C.K YOU":        "******* YOU",
		"sh1t":               "****",
		"$HIT":               "****",
		"PUSH IT":            "PUSH IT",
		"ASSISTANT":          "ASSISTANT",
		"CLASS ACT":          "CLASS ACT",
		"KISS MY ASS":        "KISS MY ***",
		"COCKTAIL HOUR":      "COCKTAIL HOUR",
		"COCKY DRIVER":       "COCKY DRIVER",
		"TITLE SCREEN":       "TITLE SCREEN",
		"GRAPE JUICE":        "GRAPE JUICE",
		"ARSENAL FAN":        "ARSENAL FAN",
		"SCUNTHORPE UNITED":  "********** UNITED", // in the fixture list: it is just a word
		"MISSISSIPPI":        "MISSISSIPPI",
		"YOU C U N T":        "YOU C U N T", // spaces keep words apart
		"FUCKFUCK":           "********",
		"FUCKERS":            "*******",
		"BULLSHIT":           "********",
		"DUMBASS":            "*******",
		"BULLSEYE":           "BULLSEYE",
		"":                   "",
		"***":                "***",
		"B1TCH PLEASE":       "***** PLEASE",
		"NICE RACE N1GG4":    "NICE RACE *****",
		"ASSES":              "*****",
		"ASSET":              "ASSET",
		"CUMBERLAND":         "CUMBERLAND",
		"SPICE GIRLS":        "SPICE GIRLS",
		"HOMEWARD":           "HOMEWARD",
		"A BLOW JOB":         "A **** ***",
		"BLOW JOBS":          "BLOW JOBS", // phrases match exactly
		"XX":                 "XX",        // too short to be an entry
	}
	for in, want := range cases {
		if got := f.Censor(in); got != want {
			t.Errorf("Censor(%q) = %q, want %q", in, got, want)
		}
	}
	if got := f.Censor("YOU CUNT"); got != "YOU ****" {
		t.Errorf("got %q", got)
	}
	if n := f.Len(); n != len(testWords)-1 { // xx is too short; # is only a comment to ParseWords
		t.Errorf("filter has %d entries, want %d", n, len(testWords)-1)
	}
	// An empty filter changes nothing.
	for _, e := range []*Filter{nil, {}, NewFilter(nil)} {
		if got := e.Censor("what the fuck"); got != "what the fuck" {
			t.Errorf("empty filter: %q", got)
		}
	}
}

func TestParseWords(t *testing.T) {
	words, err := ParseWords(strings.NewReader("# a comment\r\nfuck\r\n\r\n  blow job  \nshit\n"))
	if err != nil {
		t.Fatal(err)
	}
	want := []string{"fuck", "blow job", "shit"}
	if strings.Join(words, "|") != strings.Join(want, "|") {
		t.Errorf("got %q, want %q", words, want)
	}
}

// TestLiveWords downloads the real list, as the server does, and checks it
// still covers what the Dreamcast autotest and our players expect of it.
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
	cases := map[string]string{
		"YOU ARE A SHITHEAD": "YOU ARE A ********", // the autotest's line
		"GG EVERYONE":        "GG EVERYONE",
		"HELLO FROM BOT":     "HELLO FROM BOT",
		"KISS MY ASS":        "KISS MY ***",
		"ASSISTANT":          "ASSISTANT",
		"CLASS ACT":          "CLASS ACT",
		"COCKTAIL HOUR":      "COCKTAIL HOUR",
		"TITLE SCREEN":       "TITLE SCREEN",
		"GRAPE JUICE":        "GRAPE JUICE",
		"SCUNTHORPE UNITED":  "SCUNTHORPE UNITED",
		"MISSISSIPPI":        "MISSISSIPPI",
		"NICE RACE N1GG4":    "NICE RACE *****",
		"SONIC":              "SONIC",
		"TAILS":              "TAILS",
		"KNUCKLES":           "KNUCKLES",
	}
	for in, want := range cases {
		if got := f.Censor(in); got != want {
			t.Errorf("Censor(%q) = %q, want %q", in, got, want)
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
