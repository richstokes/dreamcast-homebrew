package lobby

import (
	"bufio"
	"context"
	"errors"
	"fmt"
	"io"
	"net/http"
	"os"
	"path/filepath"
	"strings"
	"time"
)

// DefaultWordsURL is the word list the server downloads unless told
// otherwise: the English file of LDNOOBW's List of Dirty, Naughty, Obscene
// and Otherwise Bad Words, one entry per line.
const DefaultWordsURL = "https://raw.githubusercontent.com/LDNOOBW/List-of-Dirty-Naughty-Obscene-and-Otherwise-Bad-Words/master/en"

// ParseWords reads a word list: one entry per line, surrounding space
// trimmed, blank lines and lines starting with # ignored.
func ParseWords(r io.Reader) ([]string, error) {
	var words []string
	sc := bufio.NewScanner(r)
	for sc.Scan() {
		w := strings.TrimSpace(sc.Text())
		if w == "" || strings.HasPrefix(w, "#") {
			continue
		}
		words = append(words, w)
	}
	return words, sc.Err()
}

// FetchWords downloads the word list at url and returns its entries. With a
// cache path, the download is saved there and read back when a download
// fails, so a restart while the list's host is down keeps the filter it had.
// The returned string says where the list came from, for the log.
func FetchWords(ctx context.Context, url, cache string) ([]string, string, error) {
	body, err := download(ctx, url)
	if err == nil {
		words, perr := ParseWords(strings.NewReader(body))
		if perr == nil && len(words) == 0 {
			perr = errors.New("empty list")
		}
		if perr != nil {
			err = fmt.Errorf("%s: %v", url, perr)
		} else {
			if cache != "" {
				if werr := writeFile(cache, body); werr != nil {
					return words, url + " (cache not written: " + werr.Error() + ")", nil
				}
			}
			return words, url, nil
		}
	}
	if cache == "" {
		return nil, "", err
	}
	f, cerr := os.Open(cache)
	if cerr != nil {
		return nil, "", fmt.Errorf("%v; no cached copy: %v", err, cerr)
	}
	defer f.Close()
	words, perr := ParseWords(f)
	if perr != nil {
		return nil, "", fmt.Errorf("%v; cached copy: %v", err, perr)
	}
	return words, fmt.Sprintf("%s (cached copy: %v)", cache, err), nil
}

func download(ctx context.Context, url string) (string, error) {
	ctx, cancel := context.WithTimeout(ctx, 30*time.Second)
	defer cancel()
	req, err := http.NewRequestWithContext(ctx, http.MethodGet, url, nil)
	if err != nil {
		return "", err
	}
	resp, err := http.DefaultClient.Do(req)
	if err != nil {
		return "", err
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusOK {
		return "", fmt.Errorf("%s: %s", url, resp.Status)
	}
	b, err := io.ReadAll(io.LimitReader(resp.Body, 1<<20))
	if err != nil {
		return "", fmt.Errorf("%s: %v", url, err)
	}
	return string(b), nil
}

// writeFile replaces path atomically.
func writeFile(path, body string) error {
	tmp, err := os.CreateTemp(filepath.Dir(path), ".bad-words-*")
	if err != nil {
		return err
	}
	if _, err := tmp.WriteString(body); err == nil {
		err = tmp.Close()
	} else {
		tmp.Close()
	}
	if err != nil {
		os.Remove(tmp.Name())
		return err
	}
	if err := os.Rename(tmp.Name(), path); err != nil {
		os.Remove(tmp.Name())
		return err
	}
	return nil
}
