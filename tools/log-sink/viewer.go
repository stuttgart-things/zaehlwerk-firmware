package main

import (
	"bufio"
	"encoding/json"
	"fmt"
	"html"
	"net/http"
	"os"
	"path/filepath"
	"sort"
	"strconv"
	"strings"
)

// The viewer exists for one thing a JSONL file cannot do: show the signal
// around a crossing next to the thresholds that judged it. Everything else here
// is scaffolding to get to that picture.

const viewerCSS = `
:root{--ink:#1B2430;--bg:#EEF1F4;--card:#fff;--line:#C4CDD6;--muted:#5C6B7A;
--orange:#FF6B2C;--teal:#2A9D8F}
*{box-sizing:border-box;margin:0;padding:0}
body{background:var(--bg);color:var(--ink);font:14px/1.5 system-ui,sans-serif;padding:20px}
.wrap{max-width:1000px;margin:0 auto}
h1{font-size:18px;margin-bottom:4px}
h2{font-size:11px;letter-spacing:.14em;text-transform:uppercase;color:var(--muted);margin:18px 0 8px}
a{color:#1B6FB5}
.card{background:var(--card);border:1px solid var(--line);border-radius:8px;padding:14px;margin-top:12px}
table{border-collapse:collapse;width:100%;font:12px ui-monospace,Menlo,monospace}
th{text-align:left;color:var(--muted);font-weight:600;padding:4px 8px;border-bottom:1px solid var(--line)}
td{padding:3px 8px;border-bottom:1px solid #EEF1F4;white-space:nowrap}
tr:hover td{background:#F7F9FA}
.pill{display:inline-block;padding:1px 7px;border-radius:9px;font-size:11px}
.counted{background:#E3F3F0;color:#12403A}
.below_threshold{background:#EEF1F4;color:var(--muted)}
.deadtime{background:#FFF0E8;color:#B8541F}
.ambiguous{background:#FFE9E3;color:#A33A15}
pre{font:12px ui-monospace,monospace;white-space:pre-wrap;color:var(--muted)}
.filters a{margin-right:12px;font-size:12px}
`

type viewEvent struct {
	Seq      uint64
	Type     string
	Decision string
	Side     string
	Ratio    float64
	PeakA    int
	PeakB    int
	Counted  bool
	Samples  int
	RecvAt   string
	Tag      string
	Note     string
	Player   string
	Raw      []byte
}

type sessionParams struct {
	ThresholdA int `json:"threshold_a"`
	ThresholdB int `json:"threshold_b"`
	LogA       int `json:"log_threshold_a"`
	LogB       int `json:"log_threshold_b"`
}

func readSession(path string) ([]viewEvent, sessionParams, error) {
	fh, err := os.Open(path)
	if err != nil {
		return nil, sessionParams{}, err
	}
	defer fh.Close()

	var evs []viewEvent
	var params sessionParams

	sc := bufio.NewScanner(fh)
	sc.Buffer(make([]byte, 0, 64*1024), 8*1024*1024)
	for sc.Scan() {
		line := append([]byte(nil), sc.Bytes()...)
		env, err := parseEnvelope(line)
		if err != nil {
			continue
		}
		if env.Type == "session" {
			var s struct {
				Params sessionParams `json:"params"`
			}
			if json.Unmarshal(line, &s) == nil {
				params = s.Params
			}
		}
		ev := viewEvent{Seq: env.Seq, Type: env.Type, Raw: line}
		var extra struct {
			Decision   string  `json:"decision"`
			Side       *string `json:"side"`
			Ratio      float64 `json:"ratio"`
			PeakA      int     `json:"peak_a"`
			PeakB      int     `json:"peak_b"`
			Counted    bool    `json:"counted"`
			RecvAt     string  `json:"recv_at"`
			Tag        string  `json:"tag"`
			Note       string  `json:"note"`
			PlayerName string  `json:"player_name"`
			Samples    struct {
				N int `json:"n"`
			} `json:"samples"`
		}
		if json.Unmarshal(line, &extra) == nil {
			ev.Decision, ev.Ratio = extra.Decision, extra.Ratio
			ev.PeakA, ev.PeakB, ev.Counted = extra.PeakA, extra.PeakB, extra.Counted
			ev.RecvAt, ev.Samples = extra.RecvAt, extra.Samples.N
			ev.Tag, ev.Note, ev.Player = extra.Tag, extra.Note, extra.PlayerName
			if extra.Side != nil {
				ev.Side = *extra.Side
			}
		}
		evs = append(evs, ev)
	}
	return evs, params, sc.Err()
}

func page(w http.ResponseWriter, title, body string) {
	fmt.Fprintf(w, `<!doctype html><meta charset=utf-8>
<meta name=viewport content="width=device-width,initial-scale=1">
<title>%s</title><style>%s</style><div class=wrap>%s</div>`,
		html.EscapeString(title), viewerCSS, body)
}

func serveViewer(addr, dir string, sink *Sink) {
	// Sessions live under the day they were recorded, so the listing looks
	// across all of them and the file name alone still finds one.
	findSession := func(name string) string {
		m, _ := filepath.Glob(filepath.Join(dir, "*", "sessions", name))
		if len(m) > 0 {
			return m[0]
		}
		return filepath.Join(dir, "sessions", name)
	}

	http.HandleFunc("/", func(w http.ResponseWriter, r *http.Request) {
		if r.URL.Path != "/" {
			http.NotFound(w, r)
			return
		}
		entries, _ := filepath.Glob(filepath.Join(dir, "*", "sessions", "*.jsonl"))
		sort.Sort(sort.Reverse(sort.StringSlice(entries)))

		var b strings.Builder
		b.WriteString("<h1>zaehlwerk log-sink</h1>")
		for _, se := range sink.Sessions() {
			b.WriteString("<div class=card><pre>" +
				html.EscapeString(se.Summary().Text()) + "</pre></div>")
		}
		b.WriteString("<h2>Sessions</h2><div class=card><table>")
		if len(entries) == 0 {
			b.WriteString("<tr><td>nothing recorded yet</td></tr>")
		}
		for _, e := range entries {
			st, _ := os.Stat(e)
			size := int64(0)
			if st != nil {
				size = st.Size()
			}
			name := filepath.Base(e)
			fmt.Fprintf(&b, `<tr><td><a href="/session?f=%s">%s</a></td><td>%d kB</td></tr>`,
				html.EscapeString(name), html.EscapeString(name), size/1024)
		}
		b.WriteString("</table></div>")
		page(w, "log-sink", b.String())
	})

	http.HandleFunc("/session", func(w http.ResponseWriter, r *http.Request) {
		name := filepath.Base(r.URL.Query().Get("f"))
		evs, _, err := readSession(findSession(name))
		if err != nil {
			http.Error(w, err.Error(), 404)
			return
		}
		only := r.URL.Query().Get("only")

		var b strings.Builder
		fmt.Fprintf(&b, `<h1>%s</h1><p><a href="/">back</a></p>`, html.EscapeString(name))
		b.WriteString(`<div class=filters style="margin-top:10px">`)
		for _, f := range []string{"", "counted", "ambiguous", "below_threshold", "deadtime", "point", "rally", "sink"} {
			label := f
			if f == "" {
				label = "everything"
			}
			fmt.Fprintf(&b, `<a href="/session?f=%s&only=%s">%s</a>`,
				html.EscapeString(name), f, label)
		}
		b.WriteString(`</div><div class=card><table>
<tr><th>seq</th><th>type</th><th>decision</th><th>side</th><th>player</th><th>peak A</th><th>peak B</th><th>ratio</th><th>samples</th><th>tag</th><th>note</th><th></th></tr>`)

		shown := 0
		for _, e := range evs {
			if only != "" && e.Decision != only && e.Type != only &&
				!(only == "counted" && e.Counted) {
				continue
			}
			if shown >= 500 {
				break
			}
			shown++
			link := ""
			if e.Samples > 0 {
				link = fmt.Sprintf(`<a href="/event?f=%s&seq=%d">curve</a>`,
					html.EscapeString(name), e.Seq)
			}
			cls := e.Decision
			if cls == "" {
				cls = "below_threshold"
			}
			tag := ""
			if e.Tag != "" {
				tag = `<span class="pill ambiguous">` + html.EscapeString(e.Tag) + `</span>`
			}
			fmt.Fprintf(&b,
				`<tr><td>%d</td><td>%s</td><td><span class="pill %s">%s</span></td>
<td>%s</td><td>%s</td><td>%d</td><td>%d</td><td>%.2f</td><td>%d</td><td>%s</td>
<td style="white-space:normal;max-width:260px">%s</td><td>%s</td></tr>`,
				e.Seq, html.EscapeString(e.Type), html.EscapeString(cls),
				html.EscapeString(e.Decision), html.EscapeString(e.Side),
				html.EscapeString(e.Player),
				e.PeakA, e.PeakB, e.Ratio, e.Samples, tag,
				html.EscapeString(e.Note), link)
		}
		b.WriteString("</table>")
		if shown >= 500 {
			b.WriteString(`<p style="color:#B8541F;font-size:12px;margin-top:8px">
Showing the first 500 that match. Narrow it with a filter — the file has them all.</p>`)
		}
		b.WriteString("</div>")
		page(w, name, b.String())
	})

	http.HandleFunc("/event", func(w http.ResponseWriter, r *http.Request) {
		name := filepath.Base(r.URL.Query().Get("f"))
		seq, _ := strconv.ParseUint(r.URL.Query().Get("seq"), 10, 64)
		evs, params, err := readSession(findSession(name))
		if err != nil {
			http.Error(w, err.Error(), 404)
			return
		}
		for _, e := range evs {
			if e.Seq != seq {
				continue
			}
			var full struct {
				Samples struct {
					N   int   `json:"n"`
					TUs []int `json:"t_us"`
					A   []int `json:"a"`
					B   []int `json:"b"`
				} `json:"samples"`
			}
			json.Unmarshal(e.Raw, &full)

			var b strings.Builder
			fmt.Fprintf(&b, `<h1>seq %d — %s</h1><p><a href="/session?f=%s">back</a></p>`,
				e.Seq, html.EscapeString(e.Decision), html.EscapeString(name))
			b.WriteString("<div class=card>")
			b.WriteString(curveSVG(full.Samples.TUs, full.Samples.A, full.Samples.B, params))
			b.WriteString("</div><div class=card><pre>" +
				html.EscapeString(pretty(e.Raw)) + "</pre></div>")
			page(w, fmt.Sprintf("seq %d", seq), b.String())
			return
		}
		http.NotFound(w, r)
	})

	if err := http.ListenAndServe(addr, nil); err != nil {
		fmt.Println("viewer:", err)
	}
}

func pretty(raw []byte) string {
	var v any
	if json.Unmarshal(raw, &v) != nil {
		return string(raw)
	}
	out, _ := json.MarshalIndent(v, "", "  ")
	return string(out)
}

// curveSVG draws both channels against time with the thresholds that judged
// them. The thresholds are the point: a peak means nothing without the line it
// was measured against.
func curveSVG(t, a, bs []int, p sessionParams) string {
	if len(t) == 0 || len(a) != len(t) || len(bs) != len(t) {
		return `<p style="color:#5C6B7A">no samples in this event</p>`
	}
	const w, h, pad = 940, 260, 30

	maxT, maxY := 1, 1
	for i := range t {
		if t[i] > maxT {
			maxT = t[i]
		}
		if a[i] > maxY {
			maxY = a[i]
		}
		if bs[i] > maxY {
			maxY = bs[i]
		}
	}
	known := p.ThresholdA > 0 || p.ThresholdB > 0
	for _, v := range []int{p.ThresholdA, p.ThresholdB, p.LogA, p.LogB} {
		if v > maxY {
			maxY = v
		}
	}
	x := func(v int) float64 { return pad + float64(v)/float64(maxT)*(w-2*pad) }
	y := func(v int) float64 { return h - pad - float64(v)/float64(maxY)*(h-2*pad) }

	path := func(vals []int) string {
		var s strings.Builder
		for i := range vals {
			if i == 0 {
				fmt.Fprintf(&s, "M%.1f %.1f", x(t[i]), y(vals[i]))
			} else {
				fmt.Fprintf(&s, " L%.1f %.1f", x(t[i]), y(vals[i]))
			}
		}
		return s.String()
	}
	line := func(v int, colour, label string) string {
		if v <= 0 {
			return ""
		}
		return fmt.Sprintf(
			`<line x1="%.1f" y1="%.1f" x2="%.1f" y2="%.1f" stroke="%s" stroke-dasharray="4 4"/>`+
				`<text x="%.1f" y="%.1f" font-size="10" fill="%s">%s %d</text>`,
			x(0), y(v), float64(w-pad), y(v), colour,
			float64(w-pad-70), y(v)-3, colour, label, v)
	}

	// A curve without the lines it was judged against says very little, so say
	// why they are missing rather than drawing a picture that looks complete.
	missing := ""
	if !known {
		missing = `<p style="color:#B8541F;font-size:12px">No thresholds drawn: this file has no
session event, so the parameters are unknown. That happens when recording started
after the board booted — switching the sink on emits a fresh session event.</p>`
	}

	return missing + fmt.Sprintf(`<svg viewBox="0 0 %d %d" width="100%%" style="background:#fff">
<line x1="%d" y1="%d" x2="%d" y2="%d" stroke="#C4CDD6"/>
%s%s%s%s
<path d="%s" fill="none" stroke="#FF6B2C" stroke-width="1.4"/>
<path d="%s" fill="none" stroke="#2A9D8F" stroke-width="1.4"/>
<text x="%d" y="16" font-size="11" fill="#FF6B2C">channel A</text>
<text x="%d" y="16" font-size="11" fill="#2A9D8F">channel B</text>
<text x="%d" y="%d" font-size="10" fill="#5C6B7A">%d µs</text>
</svg>`,
		w, h, pad, h-pad, w-pad, h-pad,
		line(p.ThresholdA, "#FF6B2C", "thr A"), line(p.LogA, "#FFB38A", "log A"),
		line(p.ThresholdB, "#2A9D8F", "thr B"), line(p.LogB, "#8FD3C9", "log B"),
		path(a), path(bs), pad, pad+80, w-pad-50, h-10, maxT)
}
