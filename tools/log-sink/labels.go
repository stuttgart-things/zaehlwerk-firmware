package main

import (
	"bufio"
	"bytes"
	"encoding/json"
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"time"
)

// Label is a record the sink writes on somebody's say-so, never the firmware.
// Editing one appends a new record naming the one it replaces, so the file
// keeps the whole history and the export resolves it (ADR-0005).
//
// raw is the record exactly as it stands in the file. The export writes that,
// not this struct marshalled again: a field the sink does not model — or one a
// later version adds — must not disappear on the way into labels.json, which
// is how missed_hit labels once lost their position (#53).
type Label struct {
	Type      string   `json:"type"`
	LabelID   string   `json:"label_id"`
	SessionID string   `json:"session_id,omitempty"`
	Kind      string   `json:"kind"`
	Value     *string  `json:"value"`
	PointID   string   `json:"point_id,omitempty"`
	RallyID   string   `json:"rally_id,omitempty"`
	EventIDs  []string `json:"event_ids,omitempty"`
	// A missing bounce has nothing to point at, so it points between two
	// events. One end may be empty: the bounce before the first hit of a rally
	// has no event in front of it.
	Between   []string `json:"between,omitempty"`
	ApproxTUs *uint64  `json:"approx_t_us,omitempty"`
	Note      string   `json:"note,omitempty"`
	Author    string   `json:"author,omitempty"`
	// The firmware event a derived label was made from (#57). Empty for a
	// label a person set.
	DerivedFrom string `json:"derived_from,omitempty"`
	Supersedes  string `json:"supersedes,omitempty"`
	RecvAt      string `json:"recv_at,omitempty"`

	raw json.RawMessage
}

// labelValues is the vocabulary, per kind. It is closed on purpose: a label
// is counted by its value, and a value nobody planned for is a column of one.
var labelValues = map[string][]string{
	"quick_mark":       {"wrong", "right"},
	"point_correction": {"correct", "belongs_a", "belongs_b", "no_point", "rally_not_over"},
	"event_correction": {"bounce_a", "bounce_b", "net", "edge", "bat_or_body", "ghost", "crosstalk"},
	"missed_hit":       {"a", "b"},
}

func allowed(kind, value string) bool {
	for _, v := range labelValues[kind] {
		if v == value {
			return true
		}
	}
	return false
}

// validate checks a label against the vocabulary and against the session it
// is about. A label that names a point the file does not hold is one nobody
// can ever resolve, so it is refused at the door rather than counted later.
func (l *Label) validate(ix *sessionIndex, standing map[string]Label) error {
	if _, ok := labelValues[l.Kind]; !ok {
		return fmt.Errorf("unknown kind %q", l.Kind)
	}
	if l.Value == nil {
		// A deletion. It only makes sense against something still standing.
		if l.Supersedes == "" {
			return errors.New("a label without a value deletes another one and must name it in supersedes")
		}
	} else if !allowed(l.Kind, *l.Value) {
		return fmt.Errorf("%q is not a value of %s (allowed: %s)",
			*l.Value, l.Kind, strings.Join(labelValues[l.Kind], ", "))
	}
	if l.Supersedes != "" {
		if _, ok := standing[l.Supersedes]; !ok {
			return fmt.Errorf("%s is not a standing label: it does not exist or was already replaced", l.Supersedes)
		}
	}
	if l.Value == nil {
		return nil
	}

	switch l.Kind {
	case "quick_mark":
		if l.RallyID == "" {
			return errors.New("a quick mark needs the rally it was set during")
		}
	case "point_correction":
		if l.PointID == "" {
			return errors.New("a point correction needs a point_id")
		}
	case "event_correction":
		if len(l.EventIDs) == 0 {
			return errors.New("an event correction needs at least one event id")
		}
	case "missed_hit":
		if len(l.Between) != 2 || (l.Between[0] == "" && l.Between[1] == "") {
			return errors.New("a missed hit needs between: the event before and the event after, one of them may be empty")
		}
	}

	if l.PointID != "" && !ix.points[l.PointID] {
		return fmt.Errorf("no point %s in this session", l.PointID)
	}
	if l.RallyID != "" && !ix.rallies[l.RallyID] {
		return fmt.Errorf("no rally %s in this session", l.RallyID)
	}
	for _, id := range append(append([]string{}, l.EventIDs...), l.Between...) {
		if _, ok := ix.events[id]; id != "" && !ok {
			return fmt.Errorf("no event %s in this session", id)
		}
	}
	return nil
}

// sessionIndex is what a label may point at. Built from the file, because the
// file is the only thing that knows what really arrived.
type sessionIndex struct {
	sessionID string
	points    map[string]bool
	rallies   map[string]bool
	events    map[string]uint64 // event id → t_us
	labels    []Label
	lastRally string
}

func indexSession(path string) (*sessionIndex, error) {
	fh, err := os.Open(path)
	if err != nil {
		return nil, err
	}
	defer fh.Close()

	ix := &sessionIndex{
		points: map[string]bool{}, rallies: map[string]bool{},
		events: map[string]uint64{},
	}
	sc := bufio.NewScanner(fh)
	sc.Buffer(make([]byte, 0, 64*1024), 8*1024*1024)
	for sc.Scan() {
		line := sc.Bytes()
		var r struct {
			Type      string `json:"type"`
			SessionID string `json:"session_id"`
			ID        string `json:"id"`
			TUs       uint64 `json:"t_us"`
			PointID   string `json:"point_id"`
			RallyID   string `json:"rally_id"`
		}
		if json.Unmarshal(line, &r) != nil {
			continue
		}
		if r.Type == "label" {
			var l Label
			if json.Unmarshal(line, &l) == nil {
				l.raw = append(json.RawMessage(nil), line...)
				ix.labels = append(ix.labels, l)
			}
			continue
		}
		if ix.sessionID == "" && r.SessionID != "" {
			ix.sessionID = r.SessionID
		}
		if r.ID != "" {
			ix.events[r.ID] = r.TUs
		}
		if r.PointID != "" && r.Type == "point" {
			ix.points[r.PointID] = true
		}
		if r.RallyID != "" {
			ix.rallies[r.RallyID] = true
			if r.Type == "rally" || r.Type == "hit" {
				ix.lastRally = r.RallyID
			}
		}
	}
	return ix, sc.Err()
}

// standingLabels resolves the supersedes chains: what is left is every label
// nobody replaced, minus the deletions.
func standingLabels(all []Label) []Label {
	superseded := map[string]bool{}
	for _, l := range all {
		if l.Supersedes != "" {
			superseded[l.Supersedes] = true
		}
	}
	var out []Label
	for _, l := range all {
		if !superseded[l.LabelID] && l.Value != nil {
			out = append(out, l)
		}
	}
	return out
}

// nextLabelID numbers labels per file. The numbers only have to be unique
// within the session they are written into, and l-<n> reads better on a phone
// than a random string.
func nextLabelID(all []Label) string {
	highest := 0
	for _, l := range all {
		if n, err := strconv.Atoi(strings.TrimPrefix(l.LabelID, "l-")); err == nil && n > highest {
			highest = n
		}
	}
	return fmt.Sprintf("l-%d", highest+1)
}

// AppendLabel validates a label against its session and appends it to the
// session's file.
//
// It goes through the sink's lock and, while the session is still being
// recorded, through the same buffered writer as the firmware's events. Opening
// the file a second time would put a label in the middle of a half-flushed
// hit, and the file is append-only precisely so that never has to be repaired.
func (s *Sink) AppendLabel(path string, l Label, now time.Time) (Label, error) {
	s.mu.Lock()
	defer s.mu.Unlock()

	var live *Session
	for _, se := range s.sessions {
		if samePath(se.Path, path) {
			live = se
			live.file.Flush()
		}
	}

	ix, err := indexSession(path)
	if err != nil {
		return l, err
	}
	standing := map[string]Label{}
	for _, x := range standingLabels(ix.labels) {
		standing[x.LabelID] = x
	}

	// Labelling the same point or the same single event again is a change of
	// mind, not a second opinion: it replaces what was there instead of
	// leaving two labels that contradict each other in the counts. That holds
	// for a label derived from a correction at the table too — a person
	// looking at the curve afterwards knows more than the button press did.
	if l.Supersedes == "" && l.Value != nil {
		for _, x := range standing {
			if x.Kind == l.Kind && sameTarget(x, l) {
				l.Supersedes = x.LabelID
			}
		}
	}

	if l.Kind == "missed_hit" && l.ApproxTUs == nil && len(l.Between) == 2 {
		l.ApproxTUs = approxBetween(ix, l.Between[0], l.Between[1])
	}
	if l.SessionID == "" {
		l.SessionID = ix.sessionID
	}
	if err := l.validate(ix, standing); err != nil {
		return l, err
	}

	l.Type = "label"
	l.LabelID = nextLabelID(ix.labels)
	l.RecvAt = now.UTC().Format(time.RFC3339Nano)
	body, err := json.Marshal(l)
	if err != nil {
		return l, err
	}

	if live != nil {
		// Same numbering as every other record the live sink writes.
		live.Records++
		body = append(body[:len(body)-1], fmt.Sprintf(`,"recv_seq":%d}`, live.Records)...)
		live.file.Write(append(body, '\n'))
		live.file.Flush()
	} else {
		fh, err := os.OpenFile(path, os.O_WRONLY|os.O_APPEND, 0o644)
		if err != nil {
			return l, err
		}
		_, werr := fh.Write(append(body, '\n'))
		cerr := fh.Close()
		if werr != nil {
			return l, werr
		}
		if cerr != nil {
			return l, cerr
		}
	}
	l.raw = body
	return l, nil
}

// samePath compares two ways of naming one file: the viewer finds a session by
// globbing, the recorder knows it by the path it created. Missing the match
// would open the file a second time behind the recorder's buffered writer.
func samePath(a, b string) bool {
	if a == b {
		return true
	}
	aa, err1 := filepath.Abs(a)
	bb, err2 := filepath.Abs(b)
	return err1 == nil && err2 == nil && aa == bb
}

func sameTarget(a, b Label) bool {
	switch a.Kind {
	case "point_correction":
		return a.PointID == b.PointID
	case "event_correction":
		return len(a.EventIDs) == 1 && len(b.EventIDs) == 1 && a.EventIDs[0] == b.EventIDs[0]
	}
	return false
}

// approxBetween places a missing bounce halfway between its neighbours. It is
// a guess and is named as one; the label is about which events it fell
// between, the time only orders it on a curve.
func approxBetween(ix *sessionIndex, before, after string) *uint64 {
	a, okA := ix.events[before]
	b, okB := ix.events[after]
	var t uint64
	switch {
	case okA && okB:
		t = a + (b-a)/2
	case okA:
		t = a
	case okB:
		t = b
	default:
		return nil
	}
	return &t
}

// rawLabels is what labels.json is made of: the stored records, byte for byte.
func rawLabels(ls []Label) json.RawMessage {
	var b bytes.Buffer
	b.WriteString("[")
	for i, l := range ls {
		if i > 0 {
			b.WriteString(",")
		}
		b.WriteString("\n")
		var pretty bytes.Buffer
		if json.Indent(&pretty, l.raw, "  ", "  ") == nil {
			b.WriteString("  ")
			b.Write(pretty.Bytes())
		} else {
			b.Write(l.raw)
		}
	}
	if len(ls) > 0 {
		b.WriteString("\n")
	}
	b.WriteString("]\n")
	return b.Bytes()
}
