package main

import (
	"bufio"
	"encoding/json"
	"os"
)

// The timeline is a session regrouped the way somebody remembers a game: rally
// by rally, each with the point it ended in and the crossings that led there —
// the discarded ones too, because a bounce the detector threw away is exactly
// what a person is looking for when a point went wrong.

type tlHit struct {
	ID       string
	TUs      uint64
	Side     string
	Decision string
	Counted  bool
	PeakA    int
	PeakB    int
	Ratio    float64
	N        int
	T, A, B  []int
	Intended string
}

type tlPoint struct {
	ID         string
	Reason     string
	Side       string
	PlayerName string
	ScoreA     int
	ScoreB     int
	Tag        string
	Note       string
}

type tlRally struct {
	ID       string
	Sequence string
	ClosedBy string
	Hits     []tlHit
	Points   []tlPoint
	Labels   []Label
	// The names on each half while this rally was played. Labels store the
	// half; the page shows who stood there, and after a change of ends that is
	// somebody else (ADR-0006).
	NameA, NameB string
}

// Open is a "wrong" said during play that nobody has followed up with what
// was wrong.
func (r tlRally) Open() bool {
	open := false
	for _, l := range r.Labels {
		switch {
		case l.Kind == "quick_mark" && l.Value != nil && *l.Value == "wrong":
			open = true
		case l.Kind != "quick_mark":
			return false
		}
	}
	return open
}

func (r tlRally) Name(side string) string {
	switch side {
	case "A", "a":
		return r.NameA
	case "B", "b":
		return r.NameB
	}
	return side
}

type timeline struct {
	SessionID string
	Rallies   []*tlRally
	Params    sessionParams
}

func buildTimeline(path string) (*timeline, error) {
	fh, err := os.Open(path)
	if err != nil {
		return nil, err
	}
	defer fh.Close()

	tl := &timeline{}
	byID := map[string]*tlRally{}
	pointRally := map[string]*tlRally{}
	eventRally := map[string]*tlRally{}
	players := map[string]string{}
	sides := map[string]string{"A": "a", "B": "b"}
	var labels []Label

	name := func(half string) string {
		if n := players[sides[half]]; n != "" {
			return n
		}
		return half
	}
	rally := func(id string) *tlRally {
		if r, ok := byID[id]; ok {
			return r
		}
		r := &tlRally{ID: id, NameA: name("A"), NameB: name("B")}
		byID[id] = r
		tl.Rallies = append(tl.Rallies, r)
		return r
	}

	sc := bufio.NewScanner(fh)
	sc.Buffer(make([]byte, 0, 64*1024), 8*1024*1024)
	for sc.Scan() {
		line := sc.Bytes()
		var m struct {
			Type       string            `json:"type"`
			SessionID  string            `json:"session_id"`
			ID         string            `json:"id"`
			TUs        uint64            `json:"t_us"`
			RallyID    string            `json:"rally_id"`
			PointID    string            `json:"point_id"`
			Phase      string            `json:"phase"`
			Sequence   string            `json:"sequence"`
			ClosedBy   string            `json:"closed_by"`
			Side       *string           `json:"side"`
			Decision   string            `json:"decision"`
			Counted    bool              `json:"counted"`
			PeakA      int               `json:"peak_a"`
			PeakB      int               `json:"peak_b"`
			Ratio      float64           `json:"ratio"`
			Reason     string            `json:"reason"`
			PlayerName string            `json:"player_name"`
			Tag        string            `json:"tag"`
			Note       string            `json:"note"`
			Players    map[string]string `json:"players"`
			Sides      map[string]string `json:"sides"`
			Params     sessionParams     `json:"params"`
			To         struct {
				A int `json:"a"`
				B int `json:"b"`
			} `json:"to"`
			Intended *struct {
				Type string `json:"type"`
			} `json:"intended"`
			Samples struct {
				N   int   `json:"n"`
				TUs []int `json:"t_us"`
				A   []int `json:"a"`
				B   []int `json:"b"`
			} `json:"samples"`
		}
		if json.Unmarshal(line, &m) != nil {
			continue
		}
		if tl.SessionID == "" && m.SessionID != "" {
			tl.SessionID = m.SessionID
		}

		switch m.Type {
		case "session":
			tl.Params = m.Params
		case "match":
			if m.Players != nil {
				players = m.Players
			}
			if m.Sides != nil {
				sides = m.Sides
			}
		case "rally":
			r := rally(m.RallyID)
			if m.Phase == "end" {
				r.Sequence, r.ClosedBy = m.Sequence, m.ClosedBy
			}
		case "hit":
			r := rally(m.RallyID)
			h := tlHit{
				ID: m.ID, TUs: m.TUs, Decision: m.Decision, Counted: m.Counted,
				PeakA: m.PeakA, PeakB: m.PeakB, Ratio: m.Ratio, N: m.Samples.N,
				T: m.Samples.TUs, A: m.Samples.A, B: m.Samples.B,
			}
			if m.Side != nil {
				h.Side = *m.Side
			}
			if m.Intended != nil {
				h.Intended = m.Intended.Type
			}
			r.Hits = append(r.Hits, h)
			eventRally[m.ID] = r
		case "point":
			r := rally(m.RallyID)
			side := ""
			if m.Side != nil {
				side = *m.Side
			}
			r.Points = append(r.Points, tlPoint{
				ID: m.PointID, Reason: m.Reason, Side: side, PlayerName: m.PlayerName,
				ScoreA: m.To.A, ScoreB: m.To.B, Tag: m.Tag, Note: m.Note,
			})
			pointRally[m.PointID] = r
		case "label":
			var l Label
			if json.Unmarshal(line, &l) == nil {
				l.raw = append(json.RawMessage(nil), line...)
				labels = append(labels, l)
			}
		}
	}
	if err := sc.Err(); err != nil {
		return nil, err
	}

	// A label belongs to the rally of the most specific thing it names.
	for _, l := range standingLabels(labels) {
		var r *tlRally
		if l.PointID != "" {
			r = pointRally[l.PointID]
		}
		if r == nil && l.RallyID != "" {
			r = byID[l.RallyID]
		}
		for _, id := range append(append([]string{}, l.EventIDs...), l.Between...) {
			if r == nil && id != "" {
				r = eventRally[id]
			}
		}
		if r != nil {
			r.Labels = append(r.Labels, l)
		}
	}
	return tl, nil
}
