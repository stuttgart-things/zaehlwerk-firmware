package main

import (
	"encoding/json"
	"fmt"
	"sort"
	"strings"
)

// count keeps the running tally a session summary is built from. It reads only
// the few fields it needs — an event type the sink has never heard of still
// gets stored and counted, it just does not get a column.
func (se *Session) count(raw []byte, typ string) {
	se.Counts[typ]++
	switch typ {
	case "hit":
		var h Hit
		if json.Unmarshal(raw, &h) != nil {
			return
		}
		se.Decision[h.Decision]++
		if h.Counted {
			se.Counted++
		}
		// Only a crossing that decided a side can be right or wrong about it.
		// Counting the ones that were never counted dragged a generated run
		// with 15% wrong sides down to 40% correct.
		if h.Intended != nil && h.Side != nil && *h.Side != "" {
			se.Intended++
			if *h.Side == h.Intended.Side {
				se.IntendedRight++
			}
		}
	case "point":
		se.Points++
		var p Point
		if json.Unmarshal(raw, &p) == nil {
			se.Reasons[p.Reason]++
			if p.Tag != "" {
				se.Tags[p.Tag]++
			}
			if p.PlayerName != "" && p.Reason != "undo" {
				se.Players[p.PlayerName]++
			}
		}
	case "rally":
		se.Rallies++
	}
}

// Summary is what the export carries next to the raw file: the numbers somebody
// reads before deciding whether the session is worth looking at in detail.
type Summary struct {
	SessionID string            `json:"session_id"`
	Started   string            `json:"started"`
	File      string            `json:"file"`
	Records   uint64            `json:"records"`
	FirstSeq  uint64            `json:"first_seq"`
	LastSeq   uint64            `json:"last_seq"`
	Lost      uint64            `json:"lost"`
	Late      uint64            `json:"late"`
	Dropped   uint64            `json:"incomplete_events"`
	Events    map[string]uint64 `json:"events_per_type"`
	Decisions map[string]uint64 `json:"hits_per_decision"`
	Counted   uint64            `json:"hits_counted"`
	Points    uint64            `json:"points"`
	Rallies   uint64            `json:"rallies"`
	Reasons   map[string]uint64 `json:"points_per_reason"`
	Tags      map[string]uint64 `json:"corrections_per_tag"`
	Players   map[string]uint64 `json:"points_per_player"`

	// Only meaningful for a mock session, where every generated hit says which
	// side it was meant to be. Nothing has to be labelled by hand for this one.
	Intended      uint64  `json:"mock_hits"`
	IntendedRight uint64  `json:"mock_side_correct"`
	MockAccuracy  float64 `json:"mock_side_accuracy"`
}

func (se *Session) Summary() Summary {
	s := Summary{
		SessionID: se.ID,
		Started:   se.Started.UTC().Format("2006-01-02T15:04:05Z"),
		File:      se.Path,
		Records:   se.Records,
		FirstSeq:  se.firstSeq,
		LastSeq:   se.lastSeq,
		Lost:      se.Lost,
		Late:      se.Late,
		Dropped:   se.Dropped,
		Events:    se.Counts,
		Decisions: se.Decision,
		Counted:   se.Counted,
		Points:    se.Points,
		Rallies:   se.Rallies,
		Reasons:   se.Reasons,
		Tags:      se.Tags,
		Players:   se.Players,
		Intended:  se.Intended,
	}
	s.IntendedRight = se.IntendedRight
	if se.Intended > 0 {
		s.MockAccuracy = float64(se.IntendedRight) / float64(se.Intended)
	}
	return s
}

func sortedKeys(m map[string]uint64) []string {
	out := make([]string, 0, len(m))
	for k := range m {
		out = append(out, k)
	}
	sort.Strings(out)
	return out
}

// Text renders the summary the way somebody reads it in a terminal or at the
// top of an export.
func (s Summary) Text() string {
	var b strings.Builder
	fmt.Fprintf(&b, "session %s, started %s\n", s.SessionID, s.Started)
	fmt.Fprintf(&b, "  records      %d\n", s.Records)
	fmt.Fprintf(&b, "  seq          %d..%d\n", s.FirstSeq, s.LastSeq)

	span := int64(s.LastSeq) - int64(s.FirstSeq) + 1
	if span > 0 {
		fmt.Fprintf(&b, "  lost         %d of %d (%.2f%%)\n",
			s.Lost, span, 100*float64(s.Lost)/float64(span))
	} else {
		fmt.Fprintf(&b, "  lost         %d\n", s.Lost)
	}
	if s.Late > 0 {
		fmt.Fprintf(&b, "  out of order %d\n", s.Late)
	}
	if s.Dropped > 0 {
		fmt.Fprintf(&b, "  incomplete   %d events never got all their chunks\n", s.Dropped)
	}

	if len(s.Events) > 0 {
		b.WriteString("  events\n")
		for _, k := range sortedKeys(s.Events) {
			fmt.Fprintf(&b, "    %-16s %d\n", k, s.Events[k])
		}
	}
	if len(s.Decisions) > 0 {
		b.WriteString("  crossings by decision\n")
		for _, k := range sortedKeys(s.Decisions) {
			fmt.Fprintf(&b, "    %-16s %d\n", k, s.Decisions[k])
		}
		fmt.Fprintf(&b, "    %-16s %d\n", "of those counted", s.Counted)
	}
	if len(s.Reasons) > 0 {
		b.WriteString("  points by reason\n")
		for _, k := range sortedKeys(s.Reasons) {
			fmt.Fprintf(&b, "    %-16s %d\n", k, s.Reasons[k])
		}
	}
	if len(s.Players) > 0 {
		b.WriteString("  points by player\n")
		for _, k := range sortedKeys(s.Players) {
			fmt.Fprintf(&b, "    %-16s %d\n", k, s.Players[k])
		}
	}
	if len(s.Tags) > 0 {
		// What the corrections had in common. This is the reason for asking at
		// the moment of the correction rather than afterwards.
		b.WriteString("  corrections by tag\n")
		for _, k := range sortedKeys(s.Tags) {
			fmt.Fprintf(&b, "    %-16s %d\n", k, s.Tags[k])
		}
	}
	if s.Intended > 0 {
		fmt.Fprintf(&b, "  mock: side right on %d of %d (%.1f%%)\n",
			s.IntendedRight, s.Intended, 100*s.MockAccuracy)
	}
	return b.String()
}
