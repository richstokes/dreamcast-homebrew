package lobby

import "strings"

// The lobby chat's word filter. The words come from a public list the
// server downloads when it starts (words.go), so nothing rude lives in this
// repository. Censor reads through the usual disguises: case, letter
// look-alikes (0 for O, 3 for E, @ for A, $ for S...) and dots, dashes or
// other punctuation pushed between the letters ("f.u.c.k"). Spaces still
// separate words, so "push it" is not read as one.
//
// Entries match whole words, so "assist", "cocktail", "title" and
// "Scunthorpe" pass. A word also matches when it is an entry followed by a
// common ending ("shithead", "asses", "fuckers"), several entries glued
// together ("fuckfuck") or one of a few glue prefixes on either of those
// ("bullshit", "dumbass"). Entries of several words ("blow job") match as a
// phrase.

// Filter holds a word list ready for matching. The zero value censors
// nothing.
type Filter struct {
	words   map[string]bool // normalized single-word entries
	phrases []string        // normalized entries of several words
}

// endings a bad word may carry and still be the same word.
var endings = map[string]bool{
	"S": true, "ES": true, "ED": true, "ER": true, "ERS": true, "ING": true, "INGS": true,
	"HEAD": true, "HEADS": true, "FACE": true, "FACES": true, "HOLE": true, "HOLES": true,
	"BAG": true, "BAGS": true, "WIT": true, "WITS": true, "WAD": true, "WADS": true, "TARD": true, "TARDS": true,
}

// glue prefixes that make a bad word of what follows.
var glue = []string{"DUMB", "JACK", "SMART", "BULL", "HORSE", "DOG", "DIP", "MOTHER", "FAT"}

// lookalike maps the characters people type for letters.
var lookalike = map[byte]byte{
	'0': 'O', '1': 'I', '3': 'E', '4': 'A', '5': 'S', '6': 'G', '7': 'T', '8': 'B', '@': 'A', '$': 'S',
	'!': 'I', '|': 'I', '+': 'T', '(': 'C', '<': 'C', '[': 'C', '{': 'C',
}

// NewFilter builds a filter from a word list as ParseWords returns it.
// Entries are normalized like the text they match; those that come out
// shorter than three letters are dropped.
func NewFilter(words []string) *Filter {
	f := &Filter{words: map[string]bool{}}
	for _, w := range words {
		n, _ := normalize(w)
		if len(strings.ReplaceAll(n, " ", "")) < 3 {
			continue
		}
		if strings.Contains(n, " ") {
			f.phrases = append(f.phrases, n)
		} else {
			f.words[n] = true
		}
	}
	return f
}

// Len returns the number of entries the filter matches.
func (f *Filter) Len() int {
	if f == nil {
		return 0
	}
	return len(f.words) + len(f.phrases)
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

// bad reports whether the normalized word w is an entry, an entry followed
// by an ending or another bad word, or a glue prefix on a bad word.
func (f *Filter) bad(w string) bool {
	if f.words[w] {
		return true
	}
	for i := 3; i < len(w); i++ {
		if f.words[w[:i]] && (endings[w[i:]] || f.bad(w[i:])) {
			return true
		}
	}
	for _, g := range glue {
		if len(w) > len(g) && strings.HasPrefix(w, g) && f.bad(w[len(g):]) {
			return true
		}
	}
	return false
}

// Censor returns s with its bad words replaced by asterisks of the same
// length. Text with nothing to hide comes back unchanged.
func (f *Filter) Censor(s string) string {
	if f.Len() == 0 {
		return s
	}
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
	for from := 0; from < len(n); {
		to := strings.IndexByte(n[from:], ' ')
		if to < 0 {
			to = len(n)
		} else {
			to += from
		}
		if f.bad(n[from:to]) {
			mark(from, to)
		}
		from = to + 1
	}
	for _, p := range f.phrases {
		for at := 0; ; {
			i := strings.Index(n[at:], p)
			if i < 0 {
				break
			}
			from, to := at+i, at+i+len(p)
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
