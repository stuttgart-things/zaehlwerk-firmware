package main

import (
	"fmt"
	"html"
	"net/http"
	"net/url"
	"strconv"
	"strings"
	"time"
)

// The labelling pages are for the people at the table, so they speak German
// like the board's own page does. What they store is the vocabulary of
// docs/event-schema.md, in English, and the half rather than a name.

const labelCSS = `
.big{display:block;width:100%;padding:28px 12px;margin:10px 0;border:0;border-radius:12px;
font:700 26px system-ui,sans-serif;color:#fff;cursor:pointer}
.wrong{background:#C43D12}.right{background:#2A9D8F}
.rally{border-left:4px solid var(--line)}
.rally.open{border-left-color:#C43D12;background:#FFF5F1}
.rally h3{font-size:14px;margin-bottom:6px}
.btns button,.btns select{font:13px system-ui,sans-serif;padding:5px 9px;margin:2px 2px 2px 0;
border:1px solid var(--line);border-radius:6px;background:#fff;cursor:pointer}
.btns button:hover{background:#EEF1F4}
.btns input[type=text]{font:13px system-ui,sans-serif;padding:5px;width:100%;max-width:420px;
border:1px solid var(--line);border-radius:6px;margin:4px 0}
.lbl{display:inline-block;padding:2px 8px;border-radius:9px;background:#E8EEF8;color:#1B3F6B;
font-size:12px;margin:2px 4px 2px 0}
.lbl.derived{background:#F1ECF8;color:#4B2A73}
.lbl form{display:inline}
.lbl button{border:0;background:none;color:#A33A15;cursor:pointer;font-size:12px}
.hits td{vertical-align:middle;border-bottom:0}
.filters{display:flex;flex-wrap:wrap;gap:4px 12px}.filters a{margin:0}
.flash{background:#E3F3F0;color:#12403A;padding:8px 12px;border-radius:8px;margin-top:10px}
.err{background:#FFE9E3;color:#A33A15;padding:8px 12px;border-radius:8px;margin-top:10px}
details summary{cursor:pointer;color:var(--muted);font-size:12px;margin-top:6px}
`

var valueText = map[string]string{
	"wrong": "Stimmt nicht", "right": "Stimmt",
	"correct": "Punkt stimmt", "no_point": "Kein Punkt (Let, Wiederholung)",
	"rally_not_over": "Ballwechsel war noch nicht zu Ende",
	"net":            "Netz", "edge": "Kante", "bat_or_body": "Schläger, Hand oder Körper",
	"ghost": "Geistertreffer", "crosstalk": "Übersprechen vom anderen Aufsetzer",
}

// describe says what a label means, with the names that stood on each half
// during that rally.
func describe(l Label, r *tlRally) string {
	if l.Value == nil {
		return "gelöscht"
	}
	v := *l.Value
	name := func(half string) string {
		if r == nil {
			return strings.ToUpper(half)
		}
		return r.Name(strings.ToUpper(half))
	}
	switch {
	case l.Kind == "point_correction" && strings.HasPrefix(v, "belongs_"):
		return "Punkt gehörte " + name(strings.TrimPrefix(v, "belongs_"))
	case l.Kind == "event_correction" && strings.HasPrefix(v, "bounce_"):
		return "Echter Aufsetzer " + name(strings.TrimPrefix(v, "bounce_"))
	case l.Kind == "missed_hit":
		return "Fehlender Aufsetzer " + name(v)
	}
	if t, ok := valueText[v]; ok {
		return t
	}
	return v
}

func labelPage(w http.ResponseWriter, title, body string) {
	fmt.Fprintf(w, `<!doctype html><meta charset=utf-8>
<meta name=viewport content="width=device-width,initial-scale=1">
<title>%s</title><style>%s%s</style><div class=wrap>%s</div>`,
		html.EscapeString(title), viewerCSS, labelCSS, body)
}

// author is remembered per browser so a phone that labels once does not ask
// again. Optional: a label without one is still a label.
func author(r *http.Request) string {
	if c, err := r.Cookie("author"); err == nil {
		if v, err := url.QueryUnescape(c.Value); err == nil {
			return v
		}
	}
	return ""
}

func authorField(r *http.Request) string {
	return fmt.Sprintf(`<input type=text name=author placeholder="Wer labelt? (optional)" value="%s">`,
		html.EscapeString(author(r)))
}

// handleLabel is the one place a label is written. Every form on the pages
// posts here and is sent back where it came from.
func handleLabel(sink *Sink, find func(string) string) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		if r.Method != http.MethodPost {
			http.Error(w, "POST only", http.StatusMethodNotAllowed)
			return
		}
		if err := r.ParseForm(); err != nil {
			http.Error(w, err.Error(), http.StatusBadRequest)
			return
		}
		f := r.PostForm
		name := baseName(f.Get("f"))
		back := f.Get("back")
		if !strings.HasPrefix(back, "/") || strings.HasPrefix(back, "//") {
			back = "/timeline?f=" + url.QueryEscape(name)
		}

		l := Label{
			Kind: f.Get("kind"), PointID: f.Get("point_id"), RallyID: f.Get("rally_id"),
			Note: strings.TrimSpace(f.Get("note")), Supersedes: f.Get("supersedes"),
			Author: strings.TrimSpace(f.Get("author")),
		}
		if l.Author == "" {
			l.Author = author(r)
		} else {
			http.SetCookie(w, &http.Cookie{Name: "author", Value: url.QueryEscape(l.Author),
				Path: "/", MaxAge: 365 * 24 * 3600})
		}
		if f.Get("delete") == "" {
			v := f.Get("value")
			l.Value = &v
		}
		if id := f.Get("event_id"); id != "" {
			l.EventIDs = []string{id}
		}
		if l.Kind == "missed_hit" {
			l.Between = []string{f.Get("before"), f.Get("after")}
		}

		saved, err := sink.AppendLabel(find(name), l, time.Now())
		// The outcome goes into the query, in front of the #rally anchor —
		// after it, the browser would treat it as part of the anchor and the
		// page would never say whether anything was saved.
		path, anchor, _ := strings.Cut(back, "#")
		sep := "&"
		if !strings.Contains(path, "?") {
			sep = "?"
		}
		outcome := "ok=" + url.QueryEscape(saved.LabelID)
		if err != nil {
			outcome = "err=" + url.QueryEscape(err.Error())
		}
		if anchor != "" {
			anchor = "#" + anchor
		}
		http.Redirect(w, r, path+sep+outcome+anchor, http.StatusSeeOther)
	}
}

func baseName(f string) string {
	f = strings.ReplaceAll(f, "\\", "/")
	if i := strings.LastIndex(f, "/"); i >= 0 {
		f = f[i+1:]
	}
	return f
}

func flash(r *http.Request) string {
	if e := r.URL.Query().Get("err"); e != "" {
		return `<div class=err>Nicht gespeichert: ` + html.EscapeString(e) + `</div>`
	}
	if ok := r.URL.Query().Get("ok"); ok != "" {
		return `<div class=flash>Gespeichert als ` + html.EscapeString(ok) + `</div>`
	}
	return ""
}

// handleMark is the page for during play: two large buttons and nothing else
// to aim for. It marks the rally the newest live session is in right now, so
// whoever presses it does not have to know what a rally id is.
func handleMark(sink *Sink) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		var se *Session
		for _, s := range sink.Sessions() {
			se = s
		}
		var b strings.Builder
		b.WriteString(`<h1>Markieren</h1><p><a href="/">Übersicht</a></p>`)
		b.WriteString(flash(r))
		if se == nil {
			b.WriteString(`<div class=card>Gerade läuft keine Session. Sobald das Board sendet, geht es hier los.</div>`)
			labelPage(w, "Markieren", b.String())
			return
		}
		sink.mu.Lock()
		se.file.Flush()
		sink.mu.Unlock()
		ix, err := indexSession(se.Path)
		if err != nil {
			http.Error(w, err.Error(), http.StatusInternalServerError)
			return
		}
		file := baseName(se.Path)
		if ix.lastRally == "" {
			b.WriteString(`<div class=card>Noch kein Ballwechsel in dieser Session.</div>`)
			labelPage(w, "Markieren", b.String())
			return
		}
		fmt.Fprintf(&b, `<div class=card><form method=post action="/label">
<input type=hidden name=f value="%s"><input type=hidden name=kind value=quick_mark>
<input type=hidden name=rally_id value="%s"><input type=hidden name=back value="/mark">
<button class="big wrong" name=value value=wrong>Stimmt nicht</button>
<button class="big right" name=value value=right>Stimmt</button>
<div class=btns><input type=text name=note placeholder="Was war los? (optional)">%s</div>
</form>
<p style="color:var(--muted);font-size:12px;margin-top:8px">Markiert den laufenden Ballwechsel %s.
Was genau falsch war, kommt danach in die <a href="/timeline?f=%s">Zeitleiste</a>.</p></div>`,
			html.EscapeString(file), html.EscapeString(ix.lastRally), authorField(r),
			html.EscapeString(ix.lastRally), url.QueryEscape(file))
		labelPage(w, "Markieren", b.String())
	}
}

// handleTimeline is the page for after the rally: recent rallies, newest
// first, each with what the logic decided and the means to say otherwise.
func handleTimeline(find func(string) string) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		name := baseName(r.URL.Query().Get("f"))
		tl, err := buildTimeline(find(name))
		if err != nil {
			http.Error(w, err.Error(), http.StatusNotFound)
			return
		}
		n, _ := strconv.Atoi(r.URL.Query().Get("n"))
		if n <= 0 {
			n = 30
		}
		only := r.URL.Query().Get("label")
		self := "/timeline?f=" + url.QueryEscape(name)

		var b strings.Builder
		fmt.Fprintf(&b, `<h1>Zeitleiste %s</h1><p><a href="/">Übersicht</a> · <a href="/session?f=%s">alle Events</a> · <a href="/mark">Markieren</a></p>`,
			html.EscapeString(name), url.QueryEscape(name))
		b.WriteString(flash(r))

		// The filter is by what a label says, which is the error type. "open"
		// is the list of things somebody still has to look at.
		b.WriteString(`<div class=filters style="margin-top:10px">`)
		fmt.Fprintf(&b, `<a href="%s">alle</a><a href="%s&label=open">offene Marker</a><a href="%s&label=any">mit Label</a>`,
			self, self, self)
		for _, kind := range []string{"point_correction", "event_correction", "missed_hit"} {
			for _, v := range labelValues[kind] {
				fmt.Fprintf(&b, `<a href="%s&label=%s">%s</a>`, self, url.QueryEscape(v), html.EscapeString(v))
			}
		}
		b.WriteString(`</div>`)

		shown := 0
		for i := len(tl.Rallies) - 1; i >= 0 && shown < n; i-- {
			rl := tl.Rallies[i]
			if !matches(rl, only) {
				continue
			}
			shown++
			rallyCard(&b, rl, tl, name, self, r)
		}
		if shown == 0 {
			b.WriteString(`<div class=card>Nichts, was zu diesem Filter passt.</div>`)
		} else if shown == n {
			fmt.Fprintf(&b, `<p style="margin-top:10px"><a href="%s&n=%d&label=%s">ältere zeigen</a></p>`,
				self, n*3, url.QueryEscape(only))
		}
		labelPage(w, "Zeitleiste "+name, b.String())
	}
}

func matches(r *tlRally, only string) bool {
	switch only {
	case "":
		return true
	case "open":
		return r.Open()
	case "any":
		return len(r.Labels) > 0
	}
	for _, l := range r.Labels {
		if l.Value != nil && *l.Value == only {
			return true
		}
	}
	return false
}

func rallyCard(b *strings.Builder, rl *tlRally, tl *timeline, file, self string, r *http.Request) {
	cls := "card rally"
	if rl.Open() {
		cls += " open"
	}
	esc := html.EscapeString
	hidden := func(extra string) string {
		return fmt.Sprintf(`<input type=hidden name=f value="%s"><input type=hidden name=back value="%s">%s`,
			esc(file), esc(self+"#"+rl.ID), extra)
	}

	fmt.Fprintf(b, `<div class="%s" id="%s"><h3>%s`, cls, esc(rl.ID), esc(rl.ID))
	if rl.Sequence != "" {
		fmt.Fprintf(b, ` · %s`, esc(rl.Sequence))
	}
	fmt.Fprintf(b, ` · A: %s · B: %s`, esc(rl.NameA), esc(rl.NameB))
	if rl.Open() {
		b.WriteString(` · <span class="pill ambiguous">offener Marker</span>`)
	}
	b.WriteString(`</h3>`)

	if len(rl.Labels) > 0 {
		b.WriteString(`<div>`)
		for _, l := range rl.Labels {
			cls := "lbl"
			if l.DerivedFrom != "" {
				cls += " derived"
			}
			fmt.Fprintf(b, `<span class="%s" title="%s %s">%s`, cls, esc(l.LabelID), esc(l.Author), esc(describe(l, rl)))
			if l.Note != "" {
				fmt.Fprintf(b, ` — „%s“`, esc(l.Note))
			}
			fmt.Fprintf(b, `<form method=post action="/label">%s<input type=hidden name=kind value="%s">
<input type=hidden name=supersedes value="%s"><input type=hidden name=delete value=1>
<button title="Label löschen">✕</button></form></span>`, hidden(""), esc(l.Kind), esc(l.LabelID))
		}
		b.WriteString(`</div>`)
	}

	for _, p := range rl.Points {
		if p.Reason == "undo" {
			fmt.Fprintf(b, `<p style="margin-top:8px">Zurückgenommen (%s) — %d:%d</p>`, esc(p.ID), p.ScoreA, p.ScoreB)
			continue
		}
		who := p.PlayerName
		if who == "" {
			who = rl.Name(p.Side)
		}
		fmt.Fprintf(b, `<p style="margin-top:8px"><b>Punkt für %s</b> (%s, %s) — %d:%d`,
			esc(who), esc(p.Side), esc(p.Reason), p.ScoreA, p.ScoreB)
		if p.Tag != "" || p.Note != "" {
			fmt.Fprintf(b, ` · am Board: %s %s`, esc(p.Tag), esc(p.Note))
		}
		b.WriteString(`</p>`)
		fmt.Fprintf(b, `<form method=post action="/label" class=btns>%s
<input type=hidden name=kind value=point_correction><input type=hidden name=point_id value="%s">
<input type=hidden name=rally_id value="%s">
<button name=value value=correct>Stimmt</button>
<button name=value value=belongs_a>Gehörte %s</button>
<button name=value value=belongs_b>Gehörte %s</button>
<button name=value value=no_point>Kein Punkt</button>
<button name=value value=rally_not_over>Ballwechsel nicht zu Ende</button>
<input type=text name=note placeholder="Freitext (optional)"></form>`,
			hidden(""), esc(p.ID), esc(rl.ID), esc(rl.NameA), esc(rl.NameB))
	}
	if len(rl.Points) == 0 {
		b.WriteString(`<p style="margin-top:8px;color:var(--muted)">Kein Punkt vergeben.</p>`)
	}

	// Its own scroll box: on a phone the table is wider than the screen, and
	// letting it widen the page drags the buttons above it off the side.
	b.WriteString(`<div style="overflow-x:auto;margin-top:8px"><table class=hits><tr><th>Kurve</th><th>Entscheidung</th><th>Seite</th><th>A</th><th>B</th><th>Ratio</th></tr>`)
	before := ""
	for _, h := range rl.Hits {
		cls := h.Decision
		if cls == "" {
			cls = "below_threshold"
		}
		curve := `<span style="color:var(--muted)">—</span>`
		if h.N > 0 {
			curve = fmt.Sprintf(`<a href="/event?f=%s&id=%s">%s</a>`,
				url.QueryEscape(file), url.QueryEscape(h.ID), miniCurve(h, tl.Params))
		}
		fmt.Fprintf(b, `<tr><td>%s</td><td><span class="pill %s">%s</span></td><td>%s</td><td>%d</td><td>%d</td><td>%.2f</td></tr>
<tr><td colspan=6 style="border-bottom:1px solid var(--line)"><details><summary>korrigieren</summary>`,
			curve, esc(cls), esc(h.Decision), esc(h.Side), h.PeakA, h.PeakB, h.Ratio)
		fmt.Fprintf(b, `<form method=post action="/label" class=btns>%s
<input type=hidden name=kind value=event_correction><input type=hidden name=event_id value="%s">
<input type=hidden name=rally_id value="%s"><select name=value>
<option value=bounce_a>Echter Aufsetzer %s</option><option value=bounce_b>Echter Aufsetzer %s</option>
<option value=net>Netz</option><option value=edge>Kante</option>
<option value=bat_or_body>Schläger/Hand/Körper</option><option value=ghost>Geistertreffer</option>
<option value=crosstalk>Übersprechen</option></select><button>setzen</button>
<input type=text name=note placeholder="Freitext (optional)"></form>`,
			hidden(""), esc(h.ID), esc(rl.ID), esc(rl.NameA), esc(rl.NameB))
		missedForm(b, hidden, rl, before, h.ID, "davor")
		b.WriteString(`</details></td></tr>`)
		before = h.ID
	}
	if before != "" {
		b.WriteString(`<tr><td colspan=6>`)
		missedForm(b, hidden, rl, before, "", "danach")
		b.WriteString(`</td></tr>`)
	}
	b.WriteString(`</table></div></div>`)
}

// missedForm inserts a bounce the detector never saw, between two events. The
// position is what makes it countable; the time is only an estimate.
func missedForm(b *strings.Builder, hidden func(string) string, rl *tlRally, before, after, where string) {
	esc := html.EscapeString
	fmt.Fprintf(b, `<form method=post action="/label" class=btns><span style="font-size:12px;color:var(--muted)">fehlender Aufsetzer %s:</span> %s
<input type=hidden name=kind value=missed_hit><input type=hidden name=rally_id value="%s">
<input type=hidden name=before value="%s"><input type=hidden name=after value="%s">
<button name=value value=a>auf %s</button><button name=value value=b>auf %s</button>
<input type=text name=note placeholder="Freitext (optional)"></form>`,
		esc(where), hidden(""), esc(rl.ID), esc(before), esc(after), esc(rl.NameA), esc(rl.NameB))
}

// miniCurve is the curve at thumbnail size: enough to tell a clean bounce
// from a smear, not enough to measure anything. The full one is a tap away.
func miniCurve(h tlHit, p sessionParams) string {
	if len(h.T) == 0 || len(h.A) != len(h.T) || len(h.B) != len(h.T) {
		return "curve"
	}
	const w, ht = 140, 36
	maxT, maxY := 1, 1
	for i := range h.T {
		maxT = max(maxT, h.T[i])
		maxY = max(maxY, h.A[i], h.B[i])
	}
	maxY = max(maxY, p.ThresholdA, p.ThresholdB)
	step := max(1, len(h.T)/70)
	pts := func(v []int) string {
		var s strings.Builder
		for i := 0; i < len(v); i += step {
			fmt.Fprintf(&s, "%.1f,%.1f ", float64(h.T[i])/float64(maxT)*w,
				ht-float64(v[i])/float64(maxY)*ht)
		}
		return s.String()
	}
	thr := ""
	for _, t := range []int{p.ThresholdA, p.ThresholdB} {
		if t > 0 {
			y := ht - float64(t)/float64(maxY)*ht
			thr += fmt.Sprintf(`<line x1=0 x2=%d y1=%.1f y2=%.1f stroke="#C4CDD6" stroke-dasharray="2 2"/>`, w, y, y)
		}
	}
	return fmt.Sprintf(`<svg width=%d height=%d viewBox="0 0 %d %d" style="background:#fff">%s<polyline points="%s" fill=none stroke="#FF6B2C"/><polyline points="%s" fill=none stroke="#2A9D8F"/></svg>`,
		w, ht, w, ht, thr, pts(h.A), pts(h.B))
}
