package main

import (
	"archive/zip"
	"encoding/json"
	"io"
	"net/http"
	"net/http/httptest"
	"net/url"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"
)

// A short session: one rally won by A, then a change of ends, then a rally
// that is still being played. Pat starts on A and Ana on B.
var labelSession = []string{
	`{"v":1,"session_id":"s1","seq":1,"id":"s1-1","t_us":100,"type":"session","params":{"threshold_a":300,"threshold_b":300}}`,
	`{"v":1,"session_id":"s1","seq":2,"id":"s1-2","t_us":200,"type":"match","phase":"start","players":{"a":"Pat","b":"Ana"},"sides":{"A":"a","B":"b"}}`,
	`{"v":1,"session_id":"s1","seq":3,"id":"s1-3","t_us":1000,"type":"rally","rally_id":"s1-r1","phase":"start"}`,
	`{"v":1,"session_id":"s1","seq":4,"id":"s1-4","t_us":1000,"type":"hit","rally_id":"s1-r1","side":"A","decision":"counted","counted":true,"peak_a":900,"peak_b":100,"samples":{"n":3,"t_us":[0,10,20],"a":[0,900,10],"b":[0,100,5]}}`,
	`{"v":1,"session_id":"s1","seq":5,"id":"s1-5","t_us":5000,"type":"hit","rally_id":"s1-r1","side":null,"decision":"below_threshold","counted":false,"peak_a":50,"peak_b":40,"samples":{"n":0}}`,
	`{"v":1,"session_id":"s1","seq":6,"id":"s1-6","t_us":9000,"type":"hit","rally_id":"s1-r1","side":"B","decision":"counted","counted":true,"peak_a":80,"peak_b":800,"samples":{"n":0}}`,
	`{"v":1,"session_id":"s1","seq":7,"id":"s1-7","t_us":20000,"type":"rally","rally_id":"s1-r1","phase":"end","sequence":"AB","closed_by":"timeout"}`,
	`{"v":1,"session_id":"s1","seq":8,"id":"s1-8","t_us":20001,"type":"point","point_id":"s1-p1","rally_id":"s1-r1","reason":"last_bounce","side":"A","player":"a","player_name":"Pat","to":{"a":1,"b":0,"over":false}}`,
	`{"v":1,"session_id":"s1","seq":9,"id":"s1-9","t_us":30000,"type":"match","phase":"ends_swapped","players":{"a":"Pat","b":"Ana"},"sides":{"A":"b","B":"a"}}`,
	`{"v":1,"session_id":"s1","seq":10,"id":"s1-10","t_us":40000,"type":"rally","rally_id":"s1-r2","phase":"start"}`,
}

func writeSession(t *testing.T) string {
	t.Helper()
	path := filepath.Join(t.TempDir(), "120000-s1.jsonl")
	if err := os.WriteFile(path, []byte(strings.Join(labelSession, "\n")+"\n"), 0o644); err != nil {
		t.Fatal(err)
	}
	return path
}

func val(s string) *string { return &s }

func storedLabels(t *testing.T, path string) []Label {
	t.Helper()
	ix, err := indexSession(path)
	if err != nil {
		t.Fatal(err)
	}
	return ix.labels
}

func TestALabelIsAppendedToTheFileAndNothingElseChanges(t *testing.T) {
	path := writeSession(t)
	s, _ := newTestSink(t)
	before, _ := os.ReadFile(path)

	l, err := s.AppendLabel(path, Label{Kind: "point_correction", Value: val("belongs_b"),
		PointID: "s1-p1", RallyID: "s1-r1", Note: "Kante", Author: "pat"}, time.Now())
	if err != nil {
		t.Fatal(err)
	}
	if l.LabelID != "l-1" || l.SessionID != "s1" {
		t.Errorf("label = %+v", l)
	}
	after, _ := os.ReadFile(path)
	if !strings.HasPrefix(string(after), string(before)) {
		t.Fatal("the file was rewritten, not appended to")
	}
	if got := storedLabels(t, path); len(got) != 1 || *got[0].Value != "belongs_b" || got[0].RecvAt == "" {
		t.Errorf("stored = %+v", got)
	}
}

// While a session is being recorded the label goes through the same buffered
// writer as the board's events, so it cannot land inside a half-written hit.
func TestALabelOnALiveSessionGoesThroughItsWriter(t *testing.T) {
	s, dir := newTestSink(t)
	now := time.Now()
	for _, l := range labelSession {
		s.Handle([]byte(l), nil, now)
	}
	path := s.Sessions()[0].Path

	if _, err := s.AppendLabel(path, Label{Kind: "quick_mark", Value: val("wrong"),
		RallyID: "s1-r2"}, now); err != nil {
		t.Fatal(err)
	}
	s.Handle([]byte(`{"v":1,"session_id":"s1","seq":11,"id":"s1-11","type":"note","text":"after"}`), nil, now)
	s.Close()

	got := lines(t, dir)
	if len(got) != len(labelSession)+2 {
		t.Fatalf("expected %d records, got %d", len(labelSession)+2, len(got))
	}
	var l map[string]any
	if err := json.Unmarshal([]byte(got[len(labelSession)]), &l); err != nil {
		t.Fatalf("label record is not valid json: %v", err)
	}
	if l["type"] != "label" || l["recv_seq"].(float64) != float64(len(labelSession)+1) {
		t.Errorf("label record = %v", l)
	}
}

func TestALabelIsRefusedWhenItPointsAtNothing(t *testing.T) {
	path := writeSession(t)
	s, _ := newTestSink(t)
	for _, l := range []Label{
		{Kind: "point_correction", Value: val("correct"), PointID: "s1-p99"},
		{Kind: "point_correction", Value: val("belongs_c"), PointID: "s1-p1"},
		{Kind: "event_correction", Value: val("net")},
		{Kind: "missed_hit", Value: val("a"), Between: []string{"", ""}},
		{Kind: "quick_mark", Value: val("wrong")},
		{Kind: "point_correction", Supersedes: "l-7"},
		{Kind: "guess", Value: val("x")},
	} {
		if _, err := s.AppendLabel(path, l, time.Now()); err == nil {
			t.Errorf("accepted %+v", l)
		}
	}
	if got := storedLabels(t, path); len(got) != 0 {
		t.Errorf("a refused label was written: %+v", got)
	}
}

// Labelling the same point twice is a change of mind. Two standing labels on
// one point would count it twice, once each way.
func TestLabellingThePointAgainReplacesTheFirstLabel(t *testing.T) {
	path := writeSession(t)
	s, _ := newTestSink(t)
	for _, v := range []string{"belongs_b", "no_point"} {
		if _, err := s.AppendLabel(path, Label{Kind: "point_correction", Value: val(v),
			PointID: "s1-p1"}, time.Now()); err != nil {
			t.Fatal(err)
		}
	}
	all := storedLabels(t, path)
	if len(all) != 2 || all[1].Supersedes != "l-1" {
		t.Fatalf("expected the second to supersede the first: %+v", all)
	}
	if st := standingLabels(all); len(st) != 1 || *st[0].Value != "no_point" {
		t.Errorf("standing = %+v", st)
	}

	// A deletion is a record too, and leaves nothing standing.
	if _, err := s.AppendLabel(path, Label{Kind: "point_correction", Supersedes: "l-2"}, time.Now()); err != nil {
		t.Fatal(err)
	}
	if st := standingLabels(storedLabels(t, path)); len(st) != 0 {
		t.Errorf("deleted label still standing: %+v", st)
	}
}

// #53: labels.json was the Label struct marshalled again, and a missed_hit
// lost the only thing that said where the bounce was missing.
func TestAMissedHitKeepsItsPositionThroughTheExport(t *testing.T) {
	path := writeSession(t)
	s, _ := newTestSink(t)
	if _, err := s.AppendLabel(path, Label{Kind: "missed_hit", Value: val("b"),
		Between: []string{"s1-4", "s1-6"}, Author: "ana"}, time.Now()); err != nil {
		t.Fatal(err)
	}
	// A field this version of the sink has never heard of survives as well.
	fh, _ := os.OpenFile(path, os.O_WRONLY|os.O_APPEND, 0o644)
	fh.WriteString(`{"type":"label","label_id":"l-9","kind":"quick_mark","value":"right","rally_id":"s1-r1","video_t_ms":81234}` + "\n")
	fh.Close()

	out := filepath.Join(t.TempDir(), "out.zip")
	if err := runExport([]string{"-o", out, path}); err != nil {
		t.Fatal(err)
	}
	zr, err := zip.OpenReader(out)
	if err != nil {
		t.Fatal(err)
	}
	defer zr.Close()
	var labels []map[string]any
	for _, f := range zr.File {
		if f.Name == "labels.json" {
			rc, _ := f.Open()
			body, _ := io.ReadAll(rc)
			rc.Close()
			if err := json.Unmarshal(body, &labels); err != nil {
				t.Fatalf("labels.json: %v\n%s", err, body)
			}
		}
	}
	if len(labels) != 2 {
		t.Fatalf("labels = %v", labels)
	}
	m := labels[0]
	between, _ := m["between"].([]any)
	if len(between) != 2 || between[0] != "s1-4" || between[1] != "s1-6" {
		t.Errorf("between lost: %v", m)
	}
	// Halfway between the two hits around it.
	if m["approx_t_us"] != float64(5000) || m["author"] != "ana" || m["session_id"] != "s1" {
		t.Errorf("fields lost: %v", m)
	}
	if labels[1]["video_t_ms"] != float64(81234) {
		t.Errorf("an unknown field was dropped: %v", labels[1])
	}
}

func TestTheSummaryScoresPointsByTheirCorrections(t *testing.T) {
	path := writeSession(t)
	body := strings.Join(append(append([]string{}, labelSession...),
		`{"v":1,"session_id":"s1","seq":11,"id":"s1-11","type":"point","point_id":"s1-p2","rally_id":"s1-r2","reason":"double_bounce","side":"B","to":{"a":1,"b":1}}`,
		`{"v":1,"session_id":"s1","seq":12,"id":"s1-12","type":"point","point_id":"s1-p3","rally_id":"s1-r2","reason":"last_bounce","side":"A","to":{"a":2,"b":1}}`,
		`{"type":"label","label_id":"l-1","kind":"point_correction","value":"belongs_a","point_id":"s1-p1","rally_id":"s1-r1"}`,
		`{"type":"label","label_id":"l-2","kind":"point_correction","value":"belongs_a","point_id":"s1-p2","rally_id":"s1-r2"}`,
		`{"type":"label","label_id":"l-3","kind":"point_correction","value":"correct","point_id":"s1-p3","rally_id":"s1-r2"}`,
		`{"type":"label","label_id":"l-4","kind":"event_correction","value":"crosstalk","event_ids":["s1-6"],"rally_id":"s1-r1"}`,
	), "\n")
	os.WriteFile(path, []byte(body), 0o644)

	sum, _, err := replay(path)
	if err != nil {
		t.Fatal(err)
	}
	// p1 went to A and belonged to A; p2 went to B but belonged to A; p3 is
	// confirmed outright.
	if sum.PointsLabelled != 3 || sum.PointsRight != 2 {
		t.Errorf("points labelled %d, right %d", sum.PointsLabelled, sum.PointsRight)
	}
	if sum.Labels["event_correction:crosstalk"] != 1 || sum.Labels["point_correction:belongs_a"] != 2 {
		t.Errorf("labels per value = %v", sum.Labels)
	}
	if !strings.Contains(sum.Text(), "labelled points: right on 2 of 3") {
		t.Errorf("text summary:\n%s", sum.Text())
	}
}

func TestAQuickMarkStaysOpenUntilItsRallyGetsAnotherLabel(t *testing.T) {
	path := writeSession(t)
	s, _ := newTestSink(t)
	s.AppendLabel(path, Label{Kind: "quick_mark", Value: val("wrong"), RallyID: "s1-r1"}, time.Now())

	tl, _ := buildTimeline(path)
	if !tl.Rallies[0].Open() {
		t.Fatal("the marked rally is not open")
	}
	sum, _, _ := replay(path)
	if sum.OpenMarks != 1 {
		t.Errorf("open marks = %d", sum.OpenMarks)
	}

	s.AppendLabel(path, Label{Kind: "event_correction", Value: val("edge"),
		EventIDs: []string{"s1-6"}, RallyID: "s1-r1"}, time.Now())
	tl, _ = buildTimeline(path)
	if tl.Rallies[0].Open() {
		t.Error("a rally with a correction is still open")
	}
}

// Labels store the half. The page names who stood there during that rally,
// and after the change of ends that is somebody else.
func TestTheTimelineNamesWhoStoodOnEachHalfDuringThatRally(t *testing.T) {
	path := writeSession(t)
	tl, err := buildTimeline(path)
	if err != nil {
		t.Fatal(err)
	}
	if len(tl.Rallies) != 2 {
		t.Fatalf("rallies = %d", len(tl.Rallies))
	}
	r1, r2 := tl.Rallies[0], tl.Rallies[1]
	if r1.NameA != "Pat" || r1.NameB != "Ana" || r2.NameA != "Ana" || r2.NameB != "Pat" {
		t.Errorf("names: r1 %s/%s, r2 %s/%s", r1.NameA, r1.NameB, r2.NameA, r2.NameB)
	}
	// The discarded crossing is in the rally with the counted ones.
	if len(r1.Hits) != 3 || r1.Hits[1].Decision != "below_threshold" {
		t.Errorf("hits = %+v", r1.Hits)
	}
	if got := describe(Label{Kind: "point_correction", Value: val("belongs_b")}, r1); got != "Punkt gehörte Ana" {
		t.Errorf("describe = %q", got)
	}
}

func TestTheMarkPageMarksTheRallyInProgress(t *testing.T) {
	s, dir := newTestSink(t)
	now := time.Now()
	for _, l := range labelSession {
		s.Handle([]byte(l), nil, now)
	}
	file := filepath.Base(s.Sessions()[0].Path)
	srv := httptest.NewServer(newViewer(dir, s))
	defer srv.Close()

	res, err := http.Get(srv.URL + "/mark")
	if err != nil {
		t.Fatal(err)
	}
	page, _ := io.ReadAll(res.Body)
	res.Body.Close()
	if !strings.Contains(string(page), `name=rally_id value="s1-r2"`) {
		t.Fatalf("the mark page does not offer the running rally:\n%s", page)
	}

	client := &http.Client{CheckRedirect: func(*http.Request, []*http.Request) error {
		return http.ErrUseLastResponse
	}}
	res, err = client.PostForm(srv.URL+"/label", url.Values{
		"f": {file}, "kind": {"quick_mark"}, "value": {"wrong"},
		"rally_id": {"s1-r2"}, "back": {"/mark"}, "author": {"pat"},
	})
	if err != nil {
		t.Fatal(err)
	}
	res.Body.Close()
	if loc := res.Header.Get("Location"); !strings.HasPrefix(loc, "/mark?ok=l-1") {
		t.Errorf("redirect = %q", loc)
	}

	// From the timeline the way back carries the rally's anchor, and the
	// outcome has to land in front of it.
	res, err = client.PostForm(srv.URL+"/label", url.Values{
		"f": {file}, "kind": {"point_correction"}, "value": {"correct"},
		"point_id": {"s1-p1"}, "back": {"/timeline?f=" + file + "#s1-r1"},
	})
	if err != nil {
		t.Fatal(err)
	}
	res.Body.Close()
	if loc := res.Header.Get("Location"); loc != "/timeline?f="+file+"&ok=l-2#s1-r1" {
		t.Errorf("redirect = %q", loc)
	}

	s.Close()
	got := lines(t, dir)
	last := got[len(got)-2]
	if !strings.Contains(last, `"kind":"quick_mark"`) || !strings.Contains(last, `"author":"pat"`) {
		t.Errorf("last record = %s", last)
	}
}
