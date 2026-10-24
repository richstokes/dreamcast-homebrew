package main

import (
	"fmt"
	"strings"
	"time"

	"github.com/richstokes/dreamcast-homebrew/projects/games/dreamkarts/server/discord"
	"github.com/richstokes/dreamcast-homebrew/projects/games/dreamkarts/server/lobby"
)

const (
	joinColour = 0x2ecc71
	raceColour = 0xf39c12
)

// alert turns lobby news into a Discord message.
func alert(e lobby.Event, now time.Time) (discord.Event, discord.Embed) {
	stamp := now.UTC().Format(time.RFC3339)
	if e.Kind == lobby.EventJoin {
		return discord.LobbyJoin, discord.Embed{
			Title: fmt.Sprintf("%s joined lobby %d", e.Name, e.Lobby),
			Color: joinColour,
			Fields: []discord.Field{
				{Name: fmt.Sprintf("In the lobby (%d/%d)", len(e.Players), lobby.MaxPlayers), Value: strings.Join(e.Players, ", ")},
			},
			Timestamp: stamp,
		}
	}
	items := "On"
	if !e.Items {
		items = "Off"
	}
	return discord.RaceStart, discord.Embed{
		Title: fmt.Sprintf("Race starting in lobby %d", e.Lobby),
		Color: raceColour,
		Fields: []discord.Field{
			{Name: "Track", Value: pick(lobby.TrackNames, e.Track), Inline: true},
			{Name: "Laps", Value: fmt.Sprint(e.Laps), Inline: true},
			{Name: "Class", Value: pick(lobby.CCNames, e.CC), Inline: true},
			{Name: "Items", Value: items, Inline: true},
			{Name: fmt.Sprintf("Racers (%d)", len(e.Players)), Value: strings.Join(e.Players, ", ")},
		},
		Timestamp: stamp,
	}
}

func pick(names []string, i byte) string {
	if int(i) < len(names) {
		return names[i]
	}
	return fmt.Sprintf("#%d", i+1) // a newer console's track
}
