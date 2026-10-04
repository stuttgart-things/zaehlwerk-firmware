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
		if h.Intended != nil && h.Intended.Type != "" {
			se.Intended++
			se.ByType[h.Intended.Type]++
			if h.verdict() {
				se.IntendedRight++
				se.RightByType[h.Intended.Type]++
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
			if p.PointID != "" && p.Reason != "undo" && se.pointSides != nil {
				se.pointSides[p.PointID] = p.Side
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
	Intended      uint64            `json:"mock_hits"`
	IntendedRight uint64            `json:"mock_correct"`
	MockAccuracy  float64           `json:"mock_accuracy"`
	ByType        map[string]uint64 `json:"mock_hits_per_type"`
	RightByType   map[string]uint64 `json:"mock_correct_per_type"`

	// What people said about the session, from the labels still standing.
	// Only an export fills these in: the live sink does not read labels back.
	Labels         map[string]uint64 `json:"labels_per_value,omitempty"`
	OpenMarks      uint64            `json:"open_quick_marks"`
	PointsLabelled uint64            `json:"points_labelled"`
	PointsRight    uint64            `json:"points_labelled_right"`
	PointAccuracy  float64           `json:"point_accuracy"`
}

// countLabels adds the standing labels to a summary.
//
// A point counts as right when its correction says "correct", or names the
// half it was given to anyway. Every other point correction — the other half,
// no point, rally not over — is a point the logic got wrong. Points nobody
// labelled are not in the ratio at all: an unlabelled point is not a right one,
// it is one nobody looked at.
//
// A quick mark is open while its rally has no other label: somebody said
// "wrong" during play and nobody has yet said what was wrong.
func (s *Summary) countLabels(ls []Label, pointSides map[string]string) {
	if len(ls) == 0 {
		return
	}
	s.Labels = map[string]uint64{}
	resolved := map[string]bool{}
	for _, l := range ls {
		s.Labels[l.Kind+":"+*l.Value]++
		if l.Kind != "quick_mark" && l.RallyID != "" {
			resolved[l.RallyID] = true
		}
		if l.Kind != "point_correction" {
			continue
		}
		s.PointsLabelled++
		switch *l.Value {
		case "correct":
			s.PointsRight++
		case "belongs_a", "belongs_b":
			if strings.EqualFold(strings.TrimPrefix(*l.Value, "belongs_"), pointSides[l.PointID]) {
				s.PointsRight++
			}
		}
	}
	for _, l := range ls {
		if l.Kind == "quick_mark" && *l.Value == "wrong" && !resolved[l.RallyID] {
			s.OpenMarks++
		}
	}
	if s.PointsLabelled > 0 {
		s.PointAccuracy = float64(s.PointsRight) / float64(s.PointsLabelled)
	}
}

func (se *Session) Summary() Summary {
	s := Summary{
		SessionID:   se.ID,
		Started:     se.Started.UTC().Format("2006-01-02T15:04:05Z"),
		File:        se.Path,
		Records:     se.Records,
		FirstSeq:    se.firstSeq,
		LastSeq:     se.lastSeq,
		Lost:        se.Lost,
		Late:        se.Late,
		Dropped:     se.Dropped,
		Events:      se.Counts,
		Decisions:   se.Decision,
		Counted:     se.Counted,
		Points:      se.Points,
		Rallies:     se.Rallies,
		Reasons:     se.Reasons,
		Tags:        se.Tags,
		Players:     se.Players,
		Intended:    se.Intended,
		ByType:      se.ByType,
		RightByType: se.RightByType,
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
	if s.PointsLabelled > 0 {
		fmt.Fprintf(&b, "  labelled points: right on %d of %d (%.1f%%)\n",
			s.PointsRight, s.PointsLabelled, 100*s.PointAccuracy)
	}
	if len(s.Labels) > 0 {
		b.WriteString("  labels\n")
		for _, k := range sortedKeys(s.Labels) {
			fmt.Fprintf(&b, "    %-28s %d\n", k, s.Labels[k])
		}
		if s.OpenMarks > 0 {
			fmt.Fprintf(&b, "    %-28s %d\n", "quick marks still open", s.OpenMarks)
		}
	}
	if s.Intended > 0 {
		fmt.Fprintf(&b, "  mock: right on %d of %d (%.1f%%)\n",
			s.IntendedRight, s.Intended, 100*s.MockAccuracy)
		// The total hides which kind it got wrong, and that is the only part
		// worth acting on.
		for _, k := range sortedKeys(s.ByType) {
			fmt.Fprintf(&b, "    %-12s %d of %d\n", k, s.RightByType[k], s.ByType[k])
		}
	}
	return b.String()
}
