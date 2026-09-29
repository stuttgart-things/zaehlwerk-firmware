package main

import (
	"encoding/json"
	"flag"
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"time"
)

// A session file is what arrived; a game file is what happened. They are not the
// same shape and not the same size: a session holds every raw curve, hundreds of
// kilobytes of ADC counts, while the question "why was this point counted that
// way" needs the numbers around each crossing and what the logic did with them.
//
// So this splits a session into one file per game, drops the sample arrays by
// default and keeps everything that explains a decision. The result is small
// enough to hand to somebody — or to a model — and read in one go.

type GameHit struct {
	TUs       uint64  `json:"t_us"`
	Side      *string `json:"side"`
	Decision  string  `json:"decision"`
	Counted   bool    `json:"counted"`
	PeakA     int     `json:"peak_a"`
	PeakB     int     `json:"peak_b"`
	BaselineA int     `json:"baseline_a"`
	BaselineB int     `json:"baseline_b"`
	CrossAUs  int64   `json:"cross_a_us"`
	CrossBUs  int64   `json:"cross_b_us"`
	Ratio     float64 `json:"ratio"`
	SampleN   int     `json:"samples"`
	Intended  any     `json:"intended,omitempty"`
	Samples   any     `json:"raw,omitempty"`
}

type GamePoint struct {
	PointID    string         `json:"point_id"`
	Reason     string         `json:"reason"`
	Hint       string         `json:"hint,omitempty"`
	Side       string         `json:"side"`
	Player     string         `json:"player,omitempty"`
	PlayerName string         `json:"player_name,omitempty"`
	To         map[string]any `json:"to"`
	Tag        string         `json:"tag,omitempty"`
	Note       string         `json:"note,omitempty"`
}

type GameRally struct {
	RallyID  string     `json:"rally_id"`
	Sequence string     `json:"sequence"`
	Hits     []GameHit  `json:"hits"`
	Point    *GamePoint `json:"point,omitempty"`
}

// ParamChange is a knob turned while the game was being played. Without these
// the file would state the settings as they were at boot and describe a game
// that was played under different ones.
type ParamChange struct {
	At   string `json:"at"`
	Name string `json:"name"`
	From string `json:"from"`
	To   string `json:"to"`
	By   string `json:"by"`
}

type Game struct {
	Number    int            `json:"game"`
	SetNumber int            `json:"set_number"`
	Session   string         `json:"session_id"`
	Firmware  map[string]any `json:"firmware"`

	// The settings in force when the game started, and anything turned during
	// it. A file that cannot say which thresholds produced its numbers is a
	// file nobody can compare with another one.
	Params        map[string]any `json:"params"`
	ParamsChanged []ParamChange  `json:"params_changed"`

	Players   map[string]any `json:"players"`
	Sides     map[string]any `json:"sides"`
	Started   string         `json:"started"`
	Ended     string         `json:"ended"`
	DurationS int            `json:"duration_s"`
	Final     map[string]any `json:"final,omitempty"`

	// What the recording itself did, because a game with holes in it must not
	// read like a complete one.
	Transport map[string]uint64 `json:"transport"`

	Rallies []GameRally    `json:"rallies"`
	Loose   []GamePoint    `json:"points_outside_a_rally,omitempty"`
	Summary map[string]any `json:"summary"`
}

func str(m map[string]any, k string) string {
	if v, ok := m[k].(string); ok {
		return v
	}
	return ""
}

func num(m map[string]any, k string) int {
	if v, ok := m[k].(float64); ok {
		return int(v)
	}
	return 0
}

// splitGames walks a session and cuts it where a game ends. A point whose `to`
// says the game is over closes one; a match event starting a set closes one too,
// because that is what a new game looks like when nobody finished the last.
func splitGames(path string, withSamples bool) ([]Game, error) {
	raw, err := os.ReadFile(path)
	if err != nil {
		return nil, err
	}

	var (
		games            []Game
		cur              *Game
		curRally         *GameRally
		firmware, params map[string]any
		players, sides   map[string]any
		session          string
		lost, held       uint64
		incomplete       uint64
		records          uint64
		nummer           int
		satz             int
		letzteZeit       string
	)
	satz = 1

	// A copy per game, because a knob turned in one must not silently rewrite
	// the record of the games before it.
	kopie := func(m map[string]any) map[string]any {
		out := map[string]any{}
		for k, v := range m {
			out[k] = v
		}
		return out
	}

	start := func(at string) {
		nummer++
		cur = &Game{
			Number: nummer, SetNumber: satz, Session: session, Firmware: firmware,
			Params: kopie(params), ParamsChanged: []ParamChange{},
			Players: kopie(players), Sides: kopie(sides), Started: at,
			Transport: map[string]uint64{},
		}
		curRally = nil
	}
	finish := func(at string) {
		if cur == nil {
			return
		}
		// An unfinished game still ended, at whatever arrived last.
		if at == "" {
			at = letzteZeit
		}
		cur.Ended = at
		cur.DurationS = sekunden(cur.Started, at)
		cur.Transport["records"] = records
		cur.Transport["lost"] = lost
		cur.Transport["incomplete"] = incomplete
		cur.Transport["held_and_sent_late"] = held
		records, lost, incomplete, held = 0, 0, 0, 0
		cur.Summary = summarise(*cur)
		games = append(games, *cur)
		cur = nil
	}

	for _, line := range strings.Split(string(raw), "\n") {
		if strings.TrimSpace(line) == "" {
			continue
		}
		var m map[string]any
		if json.Unmarshal([]byte(line), &m) != nil {
			continue
		}
		at := str(m, "recv_at")
		if at != "" {
			letzteZeit = at
		}

		switch str(m, "type") {
		case "session":
			session = str(m, "session_id")
			// Carried into every game file, because a measurement that cannot
			// say which binary produced it is one nobody can repeat.
			firmware = map[string]any{
				"version": m["fw_version"], "git": m["git_hash"],
				"built": m["build_date"], "sensor": m["sensor"],
				"device": m["device_id"],
			}
			if p, ok := m["params"].(map[string]any); ok {
				params = p
			}

		case "sink":
			records++
			switch str(m, "kind") {
			case "gap":
				if v, ok := m["lost"].(float64); ok {
					lost += uint64(v)
				}
			case "incomplete", "unjoinable":
				incomplete++
			}

		case "param":
			records++
			// Applied to the running set, so the next game starts from what is
			// actually in force rather than from what booted.
			name, to := str(m, "name"), str(m, "to")
			if params == nil {
				params = map[string]any{}
			}
			if name != "" {
				params[name] = to
			}
			if cur != nil {
				cur.ParamsChanged = append(cur.ParamsChanged, ParamChange{
					At: at, Name: name, From: str(m, "from"), To: to, By: str(m, "by"),
				})
			}

		case "note":
			records++
			// The firmware says so itself when it flushes what it held.
			if strings.Contains(str(m, "text"), "held for") {
				held++
			}

		case "match":
			records++
			if p, ok := m["players"].(map[string]any); ok {
				players = p
			}
			if s, ok := m["sides"].(map[string]any); ok {
				sides = s
			}
			if str(m, "phase") == "start" {
				if n := num(m, "set_number"); n > 0 {
					satz = n
				}
				finish(at)
				start(at)
			} else if cur != nil {
				// Names or ends changed mid-game: the game carries what is in
				// force at its end, and the event itself is in the session file.
				cur.Players, cur.Sides = players, sides
			}

		case "rally":
			records++
			if cur == nil {
				start(at)
			}
			if str(m, "phase") == "start" {
				curRally = &GameRally{RallyID: str(m, "rally_id")}
			} else if curRally != nil {
				curRally.Sequence = str(m, "sequence")
			}

		case "hit":
			records++
			if cur == nil {
				start(at)
			}
			h := GameHit{
				TUs: uint64(num(m, "t_us")), Decision: str(m, "decision"),
				PeakA: num(m, "peak_a"), PeakB: num(m, "peak_b"),
				BaselineA: num(m, "baseline_a"), BaselineB: num(m, "baseline_b"),
				CrossAUs: int64(num(m, "cross_a_us")), CrossBUs: int64(num(m, "cross_b_us")),
				Intended: m["intended"],
			}
			if v, ok := m["counted"].(bool); ok {
				h.Counted = v
			}
			if v, ok := m["ratio"].(float64); ok {
				h.Ratio = v
			}
			if v, ok := m["side"].(string); ok {
				h.Side = &v
			}
			if s, ok := m["samples"].(map[string]any); ok {
				h.SampleN = num(s, "n")
				if withSamples {
					h.Samples = s
				}
			}
			if curRally != nil {
				curRally.Hits = append(curRally.Hits, h)
			} else {
				// A crossing between rallies is still worth keeping: a phantom
				// hit that started nothing is exactly the kind of thing being
				// looked for.
				curRally = &GameRally{RallyID: "(between rallies)"}
				curRally.Hits = append(curRally.Hits, h)
			}

		case "point":
			records++
			if cur == nil {
				start(at)
			}
			p := GamePoint{
				PointID: str(m, "point_id"), Reason: str(m, "reason"),
				Hint: str(m, "hint"), Side: str(m, "side"),
				Player: str(m, "player"), PlayerName: str(m, "player_name"),
				Tag: str(m, "tag"), Note: str(m, "note"),
			}
			if to, ok := m["to"].(map[string]any); ok {
				p.To = to
			}
			if curRally != nil {
				curRally.Point = &p
				cur.Rallies = append(cur.Rallies, *curRally)
				curRally = nil
			} else {
				cur.Loose = append(cur.Loose, p)
			}
			if over, ok := p.To["over"].(bool); ok && over {
				cur.Final = map[string]any{
					"a": p.To["a"], "b": p.To["b"], "winner": p.Side,
					"winner_name": p.PlayerName,
				}
				finish(at)
			}
		}
	}
	// A game that was still going when the recording stopped is still a game.
	if cur != nil {
		if curRally != nil {
			cur.Rallies = append(cur.Rallies, *curRally)
		}
		finish("")
	}
	return games, nil
}

// sekunden is deliberately forgiving: a missing or odd timestamp costs the
// duration, not the file.
func sekunden(von, bis string) int {
	a, err1 := time.Parse(time.RFC3339Nano, von)
	b, err2 := time.Parse(time.RFC3339Nano, bis)
	if err1 != nil || err2 != nil || b.Before(a) {
		return 0
	}
	return int(b.Sub(a).Seconds())
}

func summarise(g Game) map[string]any {
	dec := map[string]uint64{}
	reasons := map[string]uint64{}
	tags := map[string]uint64{}
	var hits, counted, corrections uint64
	for _, r := range g.Rallies {
		for _, h := range r.Hits {
			hits++
			dec[h.Decision]++
			if h.Counted {
				counted++
			}
		}
		if r.Point != nil {
			reasons[r.Point.Reason]++
			if r.Point.Tag != "" {
				tags[r.Point.Tag]++
				corrections++
			}
		}
	}
	for _, p := range g.Loose {
		reasons[p.Reason]++
		if p.Tag != "" {
			tags[p.Tag]++
			corrections++
		}
	}
	return map[string]any{
		"rallies": len(g.Rallies), "crossings": hits, "crossings_counted": counted,
		"crossings_by_decision": dec, "points_by_reason": reasons,
		"corrections": corrections, "corrections_by_tag": tags,
	}
}

// exportGames writes one file per game and returns what it wrote. The live sink
// and the command below both go through here: two implementations of where a
// game begins would eventually disagree, and the file on disk has to mean the
// same thing either way.
func exportGames(src, dir string, withSamples bool) ([]string, error) {
	games, err := splitGames(src, withSamples)
	if err != nil {
		return nil, err
	}
	if err := os.MkdirAll(dir, 0o755); err != nil {
		return nil, err
	}
	base := strings.TrimSuffix(filepath.Base(src), ".jsonl")
	var written []string
	for _, g := range games {
		// A stretch with nothing in it is not a game. Writing it would mean
		// a folder of files most of which say nothing happened.
		if len(g.Rallies) == 0 && len(g.Loose) == 0 {
			continue
		}
		name := filepath.Join(dir, fmt.Sprintf("%s-game%d.json", base, g.Number))
		body, err := json.MarshalIndent(g, "", "  ")
		if err != nil {
			return written, err
		}
		if err := os.WriteFile(name, body, 0o644); err != nil {
			return written, err
		}
		written = append(written, name)
	}
	return written, nil
}

func runGames(args []string) error {
	fs := flag.NewFlagSet("games", flag.ExitOnError)
	out := fs.String("o", "", "where to write (default: games/ next to the session)")
	withSamples := fs.Bool("samples", false,
		"keep the raw curves — hundreds of kilobytes, only for detection work")
	fs.Parse(args)
	if fs.NArg() != 1 {
		return fmt.Errorf("usage: log-sink games [-samples] [-o dir] <session.jsonl>")
	}
	src := fs.Arg(0)

	dir := *out
	if dir == "" {
		// Beside the session, which since the day folders means the same day.
		dir = filepath.Join(filepath.Dir(filepath.Dir(src)), "games")
	}
	written, err := exportGames(src, dir, *withSamples)
	if err != nil {
		return err
	}
	if len(written) == 0 {
		return fmt.Errorf("%s holds no game: nothing scored in it", src)
	}
	for _, w := range written {
		st, _ := os.Stat(w)
		size := int64(0)
		if st != nil {
			size = st.Size()
		}
		fmt.Printf("%s  %d kB\n", w, size/1024)
	}
	return nil
}
