package main

import (
	"fmt"
	"strings"
	"time"

	"github.com/richstokes/dreamcast-homebrew/projects/games/dreamkarts/server/alerts"
	"github.com/richstokes/dreamcast-homebrew/projects/games/dreamkarts/server/lobby"
)

const (
	joinColour = 0x2ecc71
	raceColour = 0xf39c12
)

// alert turns lobby news into an alert.
func alert(e lobby.Event, now time.Time) (alerts.Event, alerts.Message) {
	if e.Kind == lobby.EventJoin {
		return alerts.LobbyJoin, alerts.Message{
			Title: fmt.Sprintf("%s joined lobby %d", e.Name, e.Lobby),
			Color: joinColour,
			Fields: []alerts.Field{
				{Name: fmt.Sprintf("In the lobby (%d/%d)", len(e.Players), lobby.MaxPlayers), Value: strings.Join(e.Players, ", ")},
			},
			Time:    now,
			Key:     e.Name,
			Players: []string{e.Name},
		}
	}
	items := "On"
	if !e.Items {
		items = "Off"
	}
	title, laps := fmt.Sprintf("Race starting in lobby %d", e.Lobby), fmt.Sprint(e.Laps)
	if e.Track == lobby.RoamTrack {
		title, laps = fmt.Sprintf("Free Roam opening in lobby %d", e.Lobby), "Free Roam"
	}
	return alerts.RaceStart, alerts.Message{
		Title: title,
		Color: raceColour,
		Fields: []alerts.Field{
			{Name: "Track", Value: pick(lobby.TrackNames, e.Track), Inline: true},
			{Name: "Laps", Value: laps, Inline: true},
			{Name: "Class", Value: pick(lobby.CCNames, e.CC), Inline: true},
			{Name: "Items", Value: items, Inline: true},
			{Name: fmt.Sprintf("Racers (%d)", len(e.Players)), Value: strings.Join(e.Players, ", ")},
		},
		Time:    now,
		Players: e.Players,
	}
}

func pick(names []string, i byte) string {
	if int(i) < len(names) {
		return names[i]
	}
	return fmt.Sprintf("#%d", i+1) // a newer console's track
}
