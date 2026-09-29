package main

import (
	"bufio"
	"bytes"
	"fmt"
	"os"
	"path/filepath"
	"sort"
	"sync"
	"time"
)

// Sink turns datagrams into one append-only file per session.
//
// Two things it must get right, because nothing downstream can recover them:
// joining the chunks an event was split into, and noticing what never arrived.
// UDP has no retry here by design (ADR-0004), so loss that is not reported is
// loss that reads as a quiet session.
type Sink struct {
	dir string
	// How long an event may wait for its missing chunks before it is written
	// off. Long enough that a slow reassembly is not mistaken for loss.
	patience time.Duration

	mu       sync.Mutex
	sessions map[string]*Session
}

type pending struct {
	parts map[int]string
	want  int
	since time.Time
}

type Session struct {
	ID      string
	Path    string
	Started time.Time

	file *bufio.Writer
	fh   *os.File

	haveFirst bool
	firstSeq  uint64
	lastSeq   uint64

	Records  uint64
	Lost     uint64
	Late     uint64
	Dropped  uint64 // events abandoned with chunks missing
	pending  map[uint64]*pending
	Counts   map[string]uint64 // events per type
	Decision map[string]uint64 // hits per decision
	Reasons  map[string]uint64 // points per deciding rule
	Tags     map[string]uint64 // corrections per tag a person chose
	Players  map[string]uint64 // points per player, by the name in force then
	Counted  uint64
	Points   uint64
	Rallies  uint64

	// Mock sessions carry what each generated hit was meant to be, so the side
	// can be scored without anybody labelling it.
	Intended      uint64
	IntendedRight uint64
}

func NewSink(dir string, patience time.Duration) (*Sink, error) {
	if err := os.MkdirAll(filepath.Join(dir, "sessions"), 0o755); err != nil {
		return nil, err
	}
	return &Sink{dir: dir, patience: patience, sessions: map[string]*Session{}}, nil
}

func (s *Sink) session(id string, now time.Time) (*Session, error) {
	if se, ok := s.sessions[id]; ok {
		return se, nil
	}
	path := filepath.Join(s.dir, "sessions",
		fmt.Sprintf("%s-%s.jsonl", now.Format("20060102-150405"), id))
	fh, err := os.OpenFile(path, os.O_CREATE|os.O_WRONLY|os.O_APPEND, 0o644)
	if err != nil {
		return nil, err
	}
	se := &Session{
		ID: id, Path: path, Started: now,
		fh: fh, file: bufio.NewWriterSize(fh, 64*1024),
		pending:  map[uint64]*pending{},
		Counts:   map[string]uint64{},
		Decision: map[string]uint64{},
		Reasons:  map[string]uint64{},
		Tags:     map[string]uint64{},
		Players:  map[string]uint64{},
	}
	s.sessions[id] = se
	return se, nil
}

// Handle takes one datagram. It never returns an error for bad input: a
// malformed datagram is recorded and the session carries on, because stopping
// on one would lose the rest of a match.
func (s *Sink) Handle(raw []byte, now time.Time) {
	s.mu.Lock()
	defer s.mu.Unlock()

	env, err := parseEnvelope(raw)
	if err != nil || env.SessionID == "" {
		if se := s.any(); se != nil {
			se.writeSink(now, fmt.Sprintf(`"kind":"unparsable","bytes":%d`, len(raw)))
		}
		return
	}

	se, err := s.session(env.SessionID, now)
	if err != nil {
		return
	}
	if env.Chunk == nil {
		se.noteSeq(env.Seq, now)
		se.write(raw, now)
		se.count(raw, env.Type)
		return
	}

	p, ok := se.pending[env.Seq]
	if !ok {
		// Only the first chunk of an event counts towards the sequence: the
		// others repeat its number, and counting each one turned three-chunk
		// hits into a hundred phantom reorderings.
		se.noteSeq(env.Seq, now)
		p = &pending{parts: map[int]string{}, want: env.Chunk.N, since: now}
		se.pending[env.Seq] = p
	}
	p.parts[env.Chunk.I] = env.Part
	if len(p.parts) < p.want {
		return
	}

	var buf bytes.Buffer
	for i := 0; i < p.want; i++ {
		buf.WriteString(p.parts[i])
	}
	delete(se.pending, env.Seq)

	joined := buf.Bytes()
	inner, err := parseEnvelope(joined)
	if err != nil {
		se.Dropped++
		se.writeSink(now, fmt.Sprintf(`"kind":"unjoinable","seq":%d,"parts":%d`,
			env.Seq, p.want))
		return
	}
	se.write(joined, now)
	se.count(joined, inner.Type)
}

func (s *Sink) any() *Session {
	for _, se := range s.sessions {
		return se
	}
	return nil
}

// Sweep writes off events whose chunks never all turned up. Without it an
// incomplete event would sit in memory and never be mentioned, which is the
// silent loss this whole design is trying to avoid.
func (s *Sink) Sweep(now time.Time) {
	s.mu.Lock()
	defer s.mu.Unlock()
	for _, se := range s.sessions {
		for seq, p := range se.pending {
			if now.Sub(p.since) < s.patience {
				continue
			}
			delete(se.pending, seq)
			se.Dropped++
			se.writeSink(now, fmt.Sprintf(
				`"kind":"incomplete","seq":%d,"have":%d,"want":%d`,
				seq, len(p.parts), p.want))
		}
		se.file.Flush()
	}
}

func (s *Sink) Close() {
	s.mu.Lock()
	defer s.mu.Unlock()
	for _, se := range s.sessions {
		se.file.Flush()
		se.fh.Close()
	}
}

func (s *Sink) Sessions() []*Session {
	s.mu.Lock()
	defer s.mu.Unlock()
	out := make([]*Session, 0, len(s.sessions))
	for _, se := range s.sessions {
		out = append(out, se)
	}
	sort.Slice(out, func(i, j int) bool { return out[i].Started.Before(out[j].Started) })
	return out
}

// noteSeq is the loss detector.
//
// The first number seen is the baseline, never zero: the firmware counts every
// event it produced, including those it did not send because no sink was
// configured yet. Taking zero as the start would report thousands of events as
// lost on the first datagram.
func (se *Session) noteSeq(seq uint64, now time.Time) {
	if !se.haveFirst {
		se.haveFirst = true
		se.firstSeq = seq
		se.lastSeq = seq
		return
	}
	switch {
	case seq == se.lastSeq+1:
		se.lastSeq = seq
	case seq > se.lastSeq+1:
		missing := seq - se.lastSeq - 1
		se.Lost += missing
		se.writeSink(now, fmt.Sprintf(`"kind":"gap","after":%d,"before":%d,"lost":%d`,
			se.lastSeq, seq, missing))
		se.lastSeq = seq
	default:
		// A repeat or a datagram that overtook another. One sender on a local
		// network makes this rare; counting it is enough, and it is not loss.
		se.Late++
	}
}

// write stores the record verbatim with the sink's own timestamps appended, so
// the firmware's bytes stay exactly as they arrived.
func (se *Session) write(raw []byte, now time.Time) {
	body := bytes.TrimRight(raw, " \t\r\n")
	if len(body) < 2 || body[len(body)-1] != '}' {
		return
	}
	se.Records++
	se.file.Write(body[:len(body)-1])
	fmt.Fprintf(se.file, `,"recv_at":"%s","recv_seq":%d}`+"\n",
		now.UTC().Format(time.RFC3339Nano), se.Records)
}

// writeSink records something the sink itself observed. It goes in the same
// file because a session that lost packets and a session that did not must not
// look alike once the file is all anybody has.
func (se *Session) writeSink(now time.Time, fields string) {
	se.Records++
	fmt.Fprintf(se.file, `{"type":"sink",%s,"recv_at":"%s","recv_seq":%d}`+"\n",
		fields, now.UTC().Format(time.RFC3339Nano), se.Records)
}
