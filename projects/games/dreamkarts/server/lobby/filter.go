package lobby

import "strings"

// The lobby chat's word filter. Censor stars out the words below, reading
// through the usual disguises: case, letter look-alikes (0 for O, 3 for E,
// @ for A, $ for S...) and dots, dashes or other punctuation pushed between
// the letters ("f.u.c.k"). Spaces still separate words, so "push it" is not
// read as one. The short entries that also start ordinary words (ass,
// cock, tit...) only match as whole words, so "assist", "cocktail" and
// "title" pass; the rest match anywhere, so "shithead" does not.

// Matched anywhere in a word.
var censorAnywhere = []string{
	"FUCK", "SHIT", "CUNT", "BITCH", "ASSHOLE", "ARSEHOLE", "DICKHEAD", "BASTARD", "WANKER", "BOLLOCK",
	"COCKSUCK", "MOTHERFUCK", "TWAT", "WHORE", "SLUT", "NIGGER", "NIGGA", "FAGGOT", "FAGOT", "RETARD",
	"SPASTIC", "TRANNY", "WETBACK", "RAPIST", "PEDO", "PAEDO", "BLOWJOB", "HANDJOB", "DOUCHE", "DILDO",
	"JIZZ", "CUMSHOT", "VAGINA", "PENIS", "PORN", "PISSED", "PISSING", "SHAG",
}

// Innocent words that contain one of the above (the Scunthorpe problem).
var censorAllow = []string{"SCUNTHORPE", "SHITAKE", "SHIITAKE"}

// Matched only as whole words.
var censorWholeWord = []string{
	"ASS", "ASSES", "ARSE", "COCK", "COCKS", "DICK", "DICKS", "TIT", "TITS", "TITTIES", "BOOB", "BOOBS",
	"CUM", "PISS", "FAG", "FAGS", "HOMO", "HOMOS", "KIKE", "KIKES", "CHINK", "CHINKS", "SPIC", "SPICS",
	"PAKI", "PAKIS", "DYKE", "DYKES", "GOOK", "GOOKS", "BEANER", "BEANERS", "NEGRO", "NEGROS", "RAPE",
	"PUSSY", "PUSSIES", "HOE", "HOES", "NONCE",
}

// lookalike maps the characters people type for letters.
var lookalike = map[byte]byte{
	'0': 'O', '1': 'I', '3': 'E', '4': 'A', '5': 'S', '6': 'G', '7': 'T', '8': 'B', '@': 'A', '$': 'S',
	'!': 'I', '|': 'I', '+': 'T', '(': 'C', '<': 'C', '[': 'C', '{': 'C',
}

// normalize rewrites s for matching: capitals, look-alikes read as their
// letter, punctuation between letters dropped and any run of spaces a
// single space. src[i] is the index in s of normalized byte i.
func normalize(s string) (string, []int) {
	var sb strings.Builder
	var src []int
	pendingSpace := false
	for i := 0; i < len(s); i++ {
		c := s[i]
		if c >= 'a' && c <= 'z' {
			c -= 32
		}
		if l, ok := lookalike[c]; ok {
			c = l
		}
		switch {
		case c >= 'A' && c <= 'Z':
			if pendingSpace && sb.Len() > 0 {
				sb.WriteByte(' ')
				src = append(src, i)
			}
			pendingSpace = false
			sb.WriteByte(c)
			src = append(src, i)
		case c == ' ':
			pendingSpace = true
		default:
			// Punctuation pushed between letters: skipped.
		}
	}
	return sb.String(), src
}

// Censor returns s with its bad words replaced by asterisks of the same
// length. A bad word inside a longer one ("shithead") takes the whole word
// with it. Text with nothing to hide comes back unchanged.
func Censor(s string) string {
	n, src := normalize(s)
	if n == "" {
		return s
	}
	hide := make([]bool, len(s))
	hit := false
	mark := func(from, to int) {
		for i := src[from]; i <= src[to-1]; i++ {
			if s[i] != ' ' {
				hide[i] = true
			}
		}
		hit = true
	}
	allowed := func(word string) bool {
		for _, a := range censorAllow {
			if word == a {
				return true
			}
		}
		return false
	}
	for _, w := range censorAnywhere {
		for at := 0; ; {
			i := strings.Index(n[at:], w)
			if i < 0 {
				break
			}
			// The whole word around the hit.
			from, to := at+i, at+i+len(w)
			for from > 0 && n[from-1] != ' ' {
				from--
			}
			for to < len(n) && n[to] != ' ' {
				to++
			}
			if !allowed(n[from:to]) {
				mark(from, to)
			}
			at = to
		}
	}
	for _, w := range censorWholeWord {
		for at := 0; ; {
			i := strings.Index(n[at:], w)
			if i < 0 {
				break
			}
			from, to := at+i, at+i+len(w)
			if (from == 0 || n[from-1] == ' ') && (to == len(n) || n[to] == ' ') {
				mark(from, to)
			}
			at = from + 1
		}
	}
	if !hit {
		return s
	}
	out := []byte(s)
	for i, h := range hide {
		if h {
			out[i] = '*'
		}
	}
	return string(out)
}
