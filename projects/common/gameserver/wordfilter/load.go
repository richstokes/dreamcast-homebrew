package wordfilter

import (
	"context"
	"os"
	"path/filepath"
	"strings"
	"time"
)

// Load downloads the bad word list at url, trying for a while before
// giving up on the download and falling back to the cached copy, then adds
// extra (comma-separated words of the server's own). Without either list
// it still returns a filter, of the extra words only: an unfiltered lobby
// beats no lobby. logf says where the words came from.
func Load(url, extra, cache string, logf func(string, ...any)) *Filter {
	var words []string
	var from string
	var err error
	for attempt := 1; ; attempt++ {
		words, from, err = FetchWords(context.Background(), url, cache)
		if err == nil || attempt == 3 {
			break
		}
		logf("bad word list: %v (retrying)", err)
		time.Sleep(5 * time.Second)
	}
	if err != nil {
		logf("bad word list: %v; starting without one", err)
		from = "nowhere"
	}
	for _, w := range strings.Split(extra, ",") {
		if w = strings.TrimSpace(w); w != "" {
			words = append(words, w)
		}
	}
	f := New(words)
	logf("bad word list: %d entries from %s", f.Len(), from)
	return f
}

// DefaultCache is a file in systemd's state directory for the service
// (StateDirectory= in the unit sets $STATE_DIRECTORY), or none.
func DefaultCache() string {
	if dir := os.Getenv("STATE_DIRECTORY"); dir != "" {
		return filepath.Join(dir, "bad-words.txt")
	}
	return ""
}
