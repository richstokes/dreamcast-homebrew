module github.com/richstokes/dreamcast-homebrew/projects/games/ridgeline/server

go 1.22

require github.com/richstokes/dreamcast-homebrew/projects/common/gameserver v0.0.0

// The servers' shared packages, from this repository.
replace github.com/richstokes/dreamcast-homebrew/projects/common/gameserver => ../../../common/gameserver
