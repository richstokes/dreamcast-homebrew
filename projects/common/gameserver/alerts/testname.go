package alerts

import "strings"

// IsTestName says whether a player name belongs to automated testing, so
// the servers leave it out of alerts: "AT" alone or followed by a digit or
// a dash. Online autotest consoles and the test bots are named that way
// (AT0042, AT-BOT, AT-RL); real names like ATOM still count.
func IsTestName(name string) bool {
	if !strings.HasPrefix(name, "AT") {
		return false
	}
	rest := name[2:]
	return rest == "" || rest[0] == '-' || (rest[0] >= '0' && rest[0] <= '9')
}
