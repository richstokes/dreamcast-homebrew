package lobby

import "testing"

func TestCensor(t *testing.T) {
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
		"TITLE SCREEN":       "TITLE SCREEN",
		"GRAPE JUICE":        "GRAPE JUICE",
		"ARSENAL FAN":        "ARSENAL FAN",
		"SCUNTHORPE UNITED":  "SCUNTHORPE UNITED",
		"MISSISSIPPI":        "MISSISSIPPI",
		"YOU C U N T":        "YOU C U N T", // spaces keep words apart
		"FUCKFUCK":           "********",
		"":                   "",
		"***":                "***",
		"B1TCH PLEASE":       "***** PLEASE",
		"NICE RACE N1GG4":    "NICE RACE *****",
		"ASSES":              "*****",
		"ASSET":              "ASSET",
		"CUMBERLAND":         "CUMBERLAND",
		"SPICE GIRLS":        "SPICE GIRLS",
		"HOMEWARD":           "HOMEWARD",
	}
	for in, want := range cases {
		if got := Censor(in); got != want {
			t.Errorf("Censor(%q) = %q, want %q", in, got, want)
		}
	}
	// Scunthorpe is left alone, but the word on its own is not.
	if got := Censor("YOU CUNT"); got != "YOU ****" {
		t.Errorf("got %q", got)
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
