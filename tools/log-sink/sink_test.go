package main

import (
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"
)

func newTestSink(t *testing.T) (*Sink, string) {
	t.Helper()
	dir := t.TempDir()
	s, err := NewSink(dir, 50*time.Millisecond)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(s.Close)
	return s, dir
}

func lines(t *testing.T, dir string) []string {
	t.Helper()
	m, _ := filepath.Glob(filepath.Join(dir, "sessions", "*.jsonl"))
	if len(m) != 1 {
		t.Fatalf("expected one session file, got %d", len(m))
	}
	b, err := os.ReadFile(m[0])
	if err != nil {
		t.Fatal(err)
	}
	return strings.Split(strings.TrimRight(string(b), "\n"), "\n")
}

func TestAWholeEventIsStoredAsItArrived(t *testing.T) {
	s, dir := newTestSink(t)
	s.Handle([]byte(`{"v":1,"session_id":"aa","seq":7,"type":"note","text":"hi"}`), time.Now())
	s.Close()

	got := lines(t, dir)
	if len(got) != 1 {
		t.Fatalf("expected one record, got %d", len(got))
	}
	var m map[string]any
	if err := json.Unmarshal([]byte(got[0]), &m); err != nil {
		t.Fatalf("stored record is not valid json: %v", err)
	}
	// The firmware's own fields survive untouched, and the sink's are added.
	if m["text"] != "hi" || m["seq"].(float64) != 7 {
		t.Errorf("original fields did not survive: %v", m)
	}
	if m["recv_at"] == nil || m["recv_seq"] == nil {
		t.Errorf("sink did not stamp the record: %v", m)
	}
}

func TestChunksAreJoinedIntoOneRecord(t *testing.T) {
	s, dir := newTestSink(t)
	whole := `{"v":1,"session_id":"aa","seq":9,"type":"hit","decision":"counted","counted":true}`
	for i, part := range []string{whole[:20], whole[20:50], whole[50:]} {
		env, _ := json.Marshal(map[string]any{
			"v": 1, "session_id": "aa", "seq": 9,
			"chunk": map[string]int{"i": i, "n": 3}, "part": part,
		})
		s.Handle(env, time.Now())
	}
	s.Close()

	got := lines(t, dir)
	if len(got) != 1 {
		t.Fatalf("three chunks should make one record, got %d", len(got))
	}
	if !strings.Contains(got[0], `"decision":"counted"`) {
		t.Errorf("joined record lost its content: %s", got[0])
	}
}

// The firmware counts every event it produced, not every event it sent, so the
// first number a sink sees is never zero. Treating zero as the start reported
// thousands of events as lost on the very first datagram.
func TestTheFirstSequenceNumberIsTheBaselineNotZero(t *testing.T) {
	s, dir := newTestSink(t)
	s.Handle([]byte(`{"v":1,"session_id":"aa","seq":4140,"type":"note"}`), time.Now())
	s.Handle([]byte(`{"v":1,"session_id":"aa","seq":4141,"type":"note"}`), time.Now())
	s.Close()

	for _, l := range lines(t, dir) {
		if strings.Contains(l, `"kind":"gap"`) {
			t.Fatalf("reported a gap before the first event: %s", l)
		}
	}
	if got := s.Sessions()[0].Lost; got != 0 {
		t.Errorf("lost = %d, want 0", got)
	}
}

func TestAJumpInSequenceNumbersIsReportedAsLoss(t *testing.T) {
	s, dir := newTestSink(t)
	s.Handle([]byte(`{"v":1,"session_id":"aa","seq":10,"type":"note"}`), time.Now())
	s.Handle([]byte(`{"v":1,"session_id":"aa","seq":14,"type":"note"}`), time.Now())
	s.Close()

	if got := s.Sessions()[0].Lost; got != 3 {
		t.Errorf("lost = %d, want 3", got)
	}
	var found bool
	for _, l := range lines(t, dir) {
		if strings.Contains(l, `"kind":"gap"`) && strings.Contains(l, `"lost":3`) {
			found = true
		}
	}
	if !found {
		// A session that lost packets must not read like one that did not once
		// the file is all anybody has.
		t.Error("the gap was counted but not written into the file")
	}
}

func TestAnEventWhoseChunksNeverArriveIsWrittenOff(t *testing.T) {
	s, dir := newTestSink(t)
	env, _ := json.Marshal(map[string]any{
		"v": 1, "session_id": "aa", "seq": 3,
		"chunk": map[string]int{"i": 0, "n": 4}, "part": `{"type":"hit"`,
	})
	s.Handle(env, time.Now())
	s.Sweep(time.Now().Add(time.Second))
	s.Close()

	var found bool
	for _, l := range lines(t, dir) {
		if strings.Contains(l, `"kind":"incomplete"`) && strings.Contains(l, `"have":1`) {
			found = true
		}
	}
	if !found {
		t.Error("an incomplete event vanished without a word")
	}
	if got := s.Sessions()[0].Dropped; got != 1 {
		t.Errorf("dropped = %d, want 1", got)
	}
}

func TestTheSummaryCountsDecisionsAndCountedSeparately(t *testing.T) {
	s, _ := newTestSink(t)
	now := time.Now()
	s.Handle([]byte(`{"v":1,"session_id":"aa","seq":1,"type":"hit","decision":"counted","counted":true}`), now)
	s.Handle([]byte(`{"v":1,"session_id":"aa","seq":2,"type":"hit","decision":"ambiguous","counted":true}`), now)
	s.Handle([]byte(`{"v":1,"session_id":"aa","seq":3,"type":"hit","decision":"below_threshold","counted":false}`), now)

	sum := s.Sessions()[0].Summary()
	if sum.Decisions["ambiguous"] != 1 || sum.Decisions["below_threshold"] != 1 {
		t.Errorf("decisions = %v", sum.Decisions)
	}
	// An ambiguous crossing still counts today; the two numbers must not be
	// collapsed into one, or the change that makes it stop counting cannot be
	// measured.
	if sum.Counted != 2 {
		t.Errorf("counted = %d, want 2", sum.Counted)
	}
}

func TestMockSessionsScoreTheSideWithoutAnybodyLabelling(t *testing.T) {
	s, _ := newTestSink(t)
	now := time.Now()
	s.Handle([]byte(`{"v":1,"session_id":"aa","seq":1,"type":"hit","side":"A","counted":true,"intended":{"side":"A","type":"bounce"}}`), now)
	s.Handle([]byte(`{"v":1,"session_id":"aa","seq":2,"type":"hit","side":"A","counted":true,"intended":{"side":"B","type":"bounce"}}`), now)

	sum := s.Sessions()[0].Summary()
	if sum.Intended != 2 || sum.IntendedRight != 1 || sum.MockAccuracy != 0.5 {
		t.Errorf("mock scoring = %d/%d (%.2f)", sum.IntendedRight, sum.Intended, sum.MockAccuracy)
	}
}

func TestReplayResolvesSupersededLabelsAndKeepsTheLoss(t *testing.T) {
	dir := t.TempDir()
	path := filepath.Join(dir, "s.jsonl")
	body := strings.Join([]string{
		`{"v":1,"session_id":"aa","seq":1,"type":"hit","decision":"counted","counted":true}`,
		`{"type":"sink","kind":"gap","after":1,"before":5,"lost":3}`,
		`{"v":1,"session_id":"aa","seq":5,"type":"point","reason":"double_bounce","side":"B"}`,
		`{"type":"label","label_id":"l-1","kind":"point_correction","value":"belongs_a","point_id":"p1"}`,
		`{"type":"label","label_id":"l-2","kind":"point_correction","value":"no_point","point_id":"p1","supersedes":"l-1"}`,
	}, "\n")
	if err := os.WriteFile(path, []byte(body), 0o644); err != nil {
		t.Fatal(err)
	}

	sum, labels, err := replay(path)
	if err != nil {
		t.Fatal(err)
	}
	if sum.Lost != 3 {
		t.Errorf("replay lost the loss: %d", sum.Lost)
	}
	if sum.Reasons["double_bounce"] != 1 {
		t.Errorf("reasons = %v", sum.Reasons)
	}
	// Nothing is overwritten in the file; the export resolves the chain.
	if len(labels) != 1 || labels[0].LabelID != "l-2" {
		t.Errorf("expected only the surviving label, got %+v", labels)
	}
}

// Every chunk of an event repeats its sequence number. Counting each one turned
// thirty-four three-chunk hits into a hundred and seven phantom reorderings in
// the first run against real hardware.
func TestTheChunksOfOneEventCountAsOneSequenceNumber(t *testing.T) {
	s, _ := newTestSink(t)
	now := time.Now()
	s.Handle([]byte(`{"v":1,"session_id":"aa","seq":1,"type":"note"}`), now)
	whole := `{"v":1,"session_id":"aa","seq":2,"type":"hit","decision":"counted","counted":true}`
	for i, part := range []string{whole[:30], whole[30:60], whole[60:]} {
		env, _ := json.Marshal(map[string]any{
			"v": 1, "session_id": "aa", "seq": 2,
			"chunk": map[string]int{"i": i, "n": 3}, "part": part,
		})
		s.Handle(env, now)
	}
	se := s.Sessions()[0]
	if se.Late != 0 {
		t.Errorf("out of order = %d, want 0 — the chunks are one event", se.Late)
	}
	if se.Lost != 0 {
		t.Errorf("lost = %d, want 0", se.Lost)
	}
}

// Getting it right is not the same question for every kind of generated event.
// A phantom hit that scores is a failure, however confidently it names a side;
// a net ball is two halves equally loud, and a detector naming a side there is
// guessing. Scoring everything by the side alone reported a run as perfect
// while a ghost had been counted as a point.
func TestEachKindOfGeneratedEventIsScoredByItsOwnRightAnswer(t *testing.T) {
	s, _ := newTestSink(t)
	now := time.Now()
	hit := func(seq int, side, decision string, counted bool, kind, intended string) {
		sideJSON := "null"
		if side != "" {
			sideJSON = `"` + side + `"`
		}
		s.Handle([]byte(fmt.Sprintf(
			`{"v":1,"session_id":"aa","seq":%d,"type":"hit","side":%s,"decision":%q,`+
				`"counted":%t,"intended":{"side":%q,"type":%q}}`,
			seq, sideJSON, decision, counted, intended, kind)), now)
	}

	hit(1, "A", "counted", true, "bounce", "A")        // right
	hit(2, "B", "counted", true, "bounce", "A")        // wrong side
	hit(3, "A", "counted", true, "ghost", "A")         // a phantom that scored
	hit(4, "", "below_threshold", false, "ghost", "A") // correctly ignored
	hit(5, "A", "ambiguous", true, "net", "A")         // honest about not knowing
	hit(6, "A", "counted", true, "net", "A")           // claimed a side anyway

	// A crossing this generator invented, with no ground truth behind it.
	s.Handle([]byte(`{"v":1,"session_id":"aa","seq":7,"type":"hit","side":null,"decision":"deadtime","counted":false}`), now)

	sum := s.Sessions()[0].Summary()
	if sum.Intended != 6 {
		t.Errorf("scored %d events, want 6 — the one with no intended must not count", sum.Intended)
	}
	if sum.IntendedRight != 3 {
		t.Errorf("right on %d, want 3", sum.IntendedRight)
	}
	for kind, want := range map[string][2]uint64{
		"bounce": {1, 2}, "ghost": {1, 2}, "net": {1, 2},
	} {
		if sum.RightByType[kind] != want[0] || sum.ByType[kind] != want[1] {
			t.Errorf("%s: %d of %d, want %d of %d",
				kind, sum.RightByType[kind], sum.ByType[kind], want[0], want[1])
		}
	}
}

// A correction without a reason is a number you cannot learn from. The tag
// comes from a fixed vocabulary precisely so the summary can group by it; free
// text alone would not cluster.
func TestCorrectionsAreCountedByTag(t *testing.T) {
	s, dir := newTestSink(t)
	now := time.Now()
	for _, tag := range []string{"net", "net", "wrong_side"} {
		s.Handle([]byte(`{"v":1,"session_id":"aa","seq":`+fmt.Sprint(len(tag))+
			`,"type":"point","reason":"manual","side":"A","tag":"`+tag+
			`","note":"Ball kam von der Kante"}`), now)
	}
	s.Handle([]byte(`{"v":1,"session_id":"aa","seq":90,"type":"point","reason":"last_bounce","side":"B"}`), now)
	s.Close()

	sum := s.Sessions()[0].Summary()
	if sum.Tags["net"] != 2 || sum.Tags["wrong_side"] != 1 {
		t.Errorf("tags = %v, want net 2 and wrong_side 1", sum.Tags)
	}
	// A point nobody corrected carries no tag and must not invent one.
	if len(sum.Tags) != 2 {
		t.Errorf("tags = %v, want exactly two kinds", sum.Tags)
	}
	// The free text survives into the file next to it.
	var found bool
	for _, l := range lines(t, dir) {
		if strings.Contains(l, "Ball kam von der Kante") {
			found = true
		}
	}
	if !found {
		t.Error("the note did not reach the file")
	}
}

// The half is what was measured; the name is what it resolved to at the time.
// Both are stored, and an undo does not award anybody a point.
func TestPointsAreCountedByThePlayerTheyResolvedTo(t *testing.T) {
	s, _ := newTestSink(t)
	now := time.Now()
	s.Handle([]byte(`{"v":1,"session_id":"aa","seq":1,"type":"point","reason":"last_bounce","side":"A","player":"a","player_name":"Pat"}`), now)
	s.Handle([]byte(`{"v":1,"session_id":"aa","seq":2,"type":"point","reason":"last_bounce","side":"B","player":"b","player_name":"Ana"}`), now)
	// After a change of ends the same half resolves to the other player.
	s.Handle([]byte(`{"v":1,"session_id":"aa","seq":3,"type":"point","reason":"last_bounce","side":"A","player":"b","player_name":"Ana"}`), now)
	s.Handle([]byte(`{"v":1,"session_id":"aa","seq":4,"type":"point","reason":"undo","side":" ","player":"","player_name":""}`), now)

	sum := s.Sessions()[0].Summary()
	if sum.Players["Ana"] != 2 || sum.Players["Pat"] != 1 {
		t.Errorf("players = %v, want Ana 2 and Pat 1", sum.Players)
	}
	if len(sum.Players) != 2 {
		t.Errorf("players = %v, an undo must not award anybody", sum.Players)
	}
}
