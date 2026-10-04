package main

import (
	"archive/zip"
	"bufio"
	"encoding/json"
	"errors"
	"flag"
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"time"
)

// replay rebuilds a summary from a stored file. The live sink has the same
// numbers in memory, but an export has to work on a file somebody copied off a
// laptop weeks later.
func replay(path string) (Summary, []Label, error) {
	fh, err := os.Open(path)
	if err != nil {
		return Summary{}, nil, err
	}
	defer fh.Close()

	se := &Session{
		Counts: map[string]uint64{}, Decision: map[string]uint64{},
		Reasons: map[string]uint64{}, Tags: map[string]uint64{},
		Players: map[string]uint64{},
		ByType:  map[string]uint64{}, RightByType: map[string]uint64{},
		pointSides: map[string]string{},
	}
	var labels []Label

	sc := bufio.NewScanner(fh)
	sc.Buffer(make([]byte, 0, 64*1024), 8*1024*1024)
	for sc.Scan() {
		line := sc.Bytes()
		if len(line) == 0 {
			continue
		}
		env, err := parseEnvelope(line)
		if err != nil {
			continue
		}
		se.Records++
		if se.ID == "" && env.SessionID != "" {
			se.ID = env.SessionID
		}

		// The sink's own arrival time is the only wall clock in the file; the
		// firmware has none. The first one is when the session started being
		// recorded, which is what a reader wants to see.
		if se.Started.IsZero() {
			var r struct {
				RecvAt string `json:"recv_at"`
			}
			if json.Unmarshal(line, &r) == nil && r.RecvAt != "" {
				if ts, err := time.Parse(time.RFC3339Nano, r.RecvAt); err == nil {
					se.Started = ts
				}
			}
		}

		switch env.Type {
		case "sink":
			// The sink's own observations carry the loss, so a replay reports
			// the same numbers as the run that recorded it.
			var s struct {
				Kind string `json:"kind"`
				Lost uint64 `json:"lost"`
			}
			if json.Unmarshal(line, &s) == nil {
				switch s.Kind {
				case "gap":
					se.Lost += s.Lost
				case "incomplete", "unjoinable":
					se.Dropped++
				}
			}
		case "label":
			var l Label
			if json.Unmarshal(line, &l) == nil {
				l.raw = append(json.RawMessage(nil), line...)
				labels = append(labels, l)
			}
		default:
			if !se.haveFirst {
				se.haveFirst, se.firstSeq = true, env.Seq
			}
			if env.Seq > se.lastSeq {
				se.lastSeq = env.Seq
			}
			se.count(line, env.Type)
		}
	}
	if err := sc.Err(); err != nil {
		return Summary{}, nil, err
	}

	live := standingLabels(labels)

	s := se.Summary()
	s.countLabels(live, se.pointSides)
	s.File = filepath.Base(path)
	// The id the board gave the session, not the file name around it, which
	// carries the time the sink first heard of it.
	if s.SessionID == "" {
		s.SessionID = strings.TrimSuffix(filepath.Base(path), ".jsonl")
	}
	return s, live, nil
}

func runExport(args []string) error {
	fs := flag.NewFlagSet("export", flag.ExitOnError)
	out := fs.String("o", "", "the zip to write (default: next to the session file)")
	fs.Parse(args)
	if fs.NArg() != 1 {
		return errors.New("usage: log-sink export [-o out.zip] <session.jsonl>")
	}
	src := fs.Arg(0)

	summary, labels, err := replay(src)
	if err != nil {
		return err
	}

	target := *out
	if target == "" {
		target = strings.TrimSuffix(src, ".jsonl") + ".zip"
	}
	zf, err := os.Create(target)
	if err != nil {
		return err
	}
	defer zf.Close()

	z := zip.NewWriter(zf)
	add := func(name string, body []byte) error {
		w, err := z.Create(name)
		if err != nil {
			return err
		}
		_, err = w.Write(body)
		return err
	}

	raw, err := os.ReadFile(src)
	if err != nil {
		return err
	}
	if err := add(filepath.Base(src), raw); err != nil {
		return err
	}

	js, _ := json.MarshalIndent(summary, "", "  ")
	if err := add("summary.json", js); err != nil {
		return err
	}
	if err := add("summary.txt", []byte(summary.Text())); err != nil {
		return err
	}

	// Labels that are still standing, with the superseded ones already resolved
	// away. The raw file above still holds every version. Written as stored,
	// not re-marshalled, so no field is lost on the way (#53).
	if err := add("labels.json", rawLabels(labels)); err != nil {
		return err
	}

	if err := z.Close(); err != nil {
		return err
	}
	fmt.Printf("%s  (%d labels, written %s)\n", target, len(labels),
		time.Now().Format("15:04:05"))
	fmt.Print(summary.Text())
	return nil
}
