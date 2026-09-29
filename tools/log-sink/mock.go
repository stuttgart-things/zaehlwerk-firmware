package main

import (
	"flag"
	"fmt"
	"math"
	"math/rand"
	"net"
	"strings"
	"time"
)

// The mock generator sends the same wire format the firmware does, so the sink
// can be exercised without a board: chunking, gaps, decisions, rallies, points.
//
// Every generated hit carries `intended`, which is what lets the summary score
// the side without anybody labelling anything. The generator deliberately gets
// some of them wrong, because a run that is always right proves only that the
// comparison is not wired up.

// The vocabulary the board's correction card offers. Kept in step with it by
// hand, which is fine while it is eight words and worth revisiting if it grows.
var mockTags = []string{"missed", "wrong_side", "ghost", "net", "edge",
	"bat_or_body", "let", "other"}

var mockNotes = []string{
	"Ball kam von der Kante zurueck",
	"beide Haelften gleichzeitig gehoert",
	"Aufschlag beruehrte das Netz",
	"Schlaeger auf dem Tisch abgelegt",
}

const mockChunkBody = 900
const mockMaxPayload = 1200

type mockSender struct {
	conn    *net.UDPConn
	session string
	seq     uint64
	loss    float64
	rnd     *rand.Rand
	sent    int
	dropped int
}

func (m *mockSender) emit(body string) {
	seq := m.seq
	m.seq++

	head := fmt.Sprintf(`{"v":1,"session_id":%q,"seq":%d,"id":"%s-%d","t_us":%d,`,
		m.session, seq, m.session, seq, time.Now().UnixMicro()%(1<<32))
	full := head + body + "}"

	if len(full) <= mockMaxPayload {
		m.write(full)
		return
	}
	n := (len(full) + mockChunkBody - 1) / mockChunkBody
	for i := 0; i < n; i++ {
		end := (i + 1) * mockChunkBody
		if end > len(full) {
			end = len(full)
		}
		part := full[i*mockChunkBody : end]
		wrap := fmt.Sprintf(`{"v":1,"session_id":%q,"seq":%d,"chunk":{"i":%d,"n":%d},"part":%q}`,
			m.session, seq, i, n, part)
		m.write(wrap)
	}
}

// write is where the loss lives. Dropping whole datagrams is what the real path
// does, so a dropped chunk leaves an event that never completes — which the
// sink has to notice rather than wait for forever.
func (m *mockSender) write(s string) {
	if m.loss > 0 && m.rnd.Float64() < m.loss {
		m.dropped++
		return
	}
	m.conn.Write([]byte(s))
	m.sent++
	time.Sleep(time.Millisecond) // do not outrun a socket buffer
}

// A bounce as the ADC would see it: a sharp rise and a decay, on both channels,
// with the far one quieter. Not physics — enough shape that a curve in the
// viewer looks like something and the ratio means what it says.
func mockSamples(near, far int, n int) (t, a, b []int) {
	for i := 0; i < n; i++ {
		us := i * 40
		env := math.Exp(-float64(i) / 18.0)
		osc := math.Sin(float64(i) / 1.7)
		t = append(t, us)
		a = append(a, int(float64(near)*env*math.Abs(osc)))
		b = append(b, int(float64(far)*env*math.Abs(osc)))
	}
	return
}

func (m *mockSender) hit(rally int, side string, decision string, counted bool,
	near, far int, samples int, intendedSide string) {

	peakA, peakB := near, far
	if side == "B" {
		peakA, peakB = far, near
	}
	ratio := 999.0
	if min(peakA, peakB) > 0 {
		ratio = float64(max(peakA, peakB)) / float64(min(peakA, peakB))
	}

	var b strings.Builder
	fmt.Fprintf(&b, `"type":"hit","rally_id":"%s-r%d","side":`, m.session, rally)
	if side == "" {
		b.WriteString("null")
	} else {
		fmt.Fprintf(&b, "%q", side)
	}
	fmt.Fprintf(&b, `,"decision":%q,"peak_a":%d,"peak_b":%d`, decision, peakA, peakB)
	fmt.Fprintf(&b, `,"baseline_a":12,"baseline_b":11,"cross_a_us":0,"cross_b_us":%d`,
		m.rnd.Intn(900))
	fmt.Fprintf(&b, `,"ratio":%.3f,"counted":%t`, ratio, counted)
	fmt.Fprintf(&b, `,"intended":{"side":%q,"type":"bounce"}`, intendedSide)

	if samples > 0 {
		ts, as, bs := mockSamples(max(peakA, peakB), min(peakA, peakB), samples)
		fmt.Fprintf(&b, `,"samples":{"pre_us":0,"n":%d,"t_us":%s,"a":%s,"b":%s}`,
			samples, intList(ts), intList(as), intList(bs))
	}
	m.emit(b.String())
}

func intList(v []int) string {
	var b strings.Builder
	b.WriteByte('[')
	for i, x := range v {
		if i > 0 {
			b.WriteByte(',')
		}
		fmt.Fprintf(&b, "%d", x)
	}
	b.WriteByte(']')
	return b.String()
}

func runMock(args []string) error {
	fs := flag.NewFlagSet("mock", flag.ExitOnError)
	to := fs.String("to", "127.0.0.1:9000", "where the sink is listening")
	rallies := fs.Int("rallies", 8, "how many rallies to play")
	loss := fs.Float64("loss", 0, "fraction of datagrams to drop, 0..1")
	wrong := fs.Float64("wrong", 0.15, "fraction of hits decided on the wrong side")
	corrections := fs.Float64("corrections", 0.3,
		"fraction of points that end as a tagged correction")
	seed := fs.Int64("seed", 1, "so a run can be repeated")
	fs.Parse(args)

	addr, err := net.ResolveUDPAddr("udp", *to)
	if err != nil {
		return err
	}
	conn, err := net.DialUDP("udp", nil, addr)
	if err != nil {
		return err
	}
	defer conn.Close()

	rnd := rand.New(rand.NewSource(*seed))
	m := &mockSender{
		conn:    conn,
		session: fmt.Sprintf("%08x", rnd.Uint32()),
		loss:    *loss,
		rnd:     rnd,
	}

	// seq 0, and the only place the parameters are stated in full.
	m.emit(`"type":"session","device_id":"mock","fw_version":"0.0.0-mock",` +
		`"git_hash":"mock","sensor":"mock","reason":"manual","params":{` +
		`"threshold_a":300,"threshold_b":300,"log_threshold_a":120,` +
		`"log_threshold_b":120,"clear_ratio":1.8,"peak_window_us":30000,` +
		`"deadtime_us":60000,"rally_timeout_ms":1500,` +
		`"pre_trigger_samples":128,"capture_samples":512}`)

	points := 0
	for r := 1; r <= *rallies; r++ {
		m.emit(fmt.Sprintf(`"type":"rally","rally_id":"%s-r%d","phase":"start"`, m.session, r))

		bounces := 2 + rnd.Intn(6)
		seqStr := ""
		for i := 0; i < bounces; i++ {
			intended := "A"
			if i%2 == 1 {
				intended = "B"
			}
			side := intended
			decision := "counted"
			// Sometimes the near channel is not much louder than the far one.
			// That is the case the ratio is for, and the one that gets it wrong.
			if rnd.Float64() < *wrong {
				decision = "ambiguous"
				if rnd.Float64() < 0.6 {
					side = map[string]string{"A": "B", "B": "A"}[intended]
				}
			}
			near := 700 + rnd.Intn(900)
			far := near / 3
			if decision == "ambiguous" {
				far = near - rnd.Intn(80)
			}
			m.hit(r, side, decision, true, near, far, 120+rnd.Intn(150), intended)
			seqStr += side

			// The crossings nobody sees: too quiet to count, or inside the
			// dead time after the one that did.
			if rnd.Float64() < 0.5 {
				m.hit(r, "", "deadtime", false, 400+rnd.Intn(300), 90, 0, intended)
			}
			if rnd.Float64() < 0.4 {
				m.hit(r, "", "below_threshold", false, 130+rnd.Intn(120), 40, 0, intended)
			}
		}

		m.emit(fmt.Sprintf(
			`"type":"rally","rally_id":"%s-r%d","phase":"end","sequence":%q,"closed_by":"timeout"`,
			m.session, r, seqStr))

		points++
		winner := "A"
		if len(seqStr) > 0 && seqStr[len(seqStr)-1] == 'A' {
			winner = "B"
		}

		// Some rallies end in a correction with a reason attached, because that
		// is what the tag vocabulary is for and an untagged run would not
		// exercise the grouping.
		reason, tag, note := "last_bounce", "", ""
		if rnd.Float64() < *corrections {
			reason = "manual"
			tag = mockTags[rnd.Intn(len(mockTags))]
			if rnd.Float64() < 0.4 {
				note = mockNotes[rnd.Intn(len(mockNotes))]
			}
		}
		extra := ""
		if tag != "" {
			extra = fmt.Sprintf(`,"tag":%q`, tag)
		}
		if note != "" {
			extra += fmt.Sprintf(`,"note":%q`, note)
		}
		m.emit(fmt.Sprintf(`"type":"point","point_id":"%s-p%d","rally_id":"%s-r%d",`+
			`"reason":%q,"hint":"","side":%q,`+
			`"from":{"a":0,"b":0,"serve":"A"},"to":{"a":1,"b":0,"serve":"B","over":false}%s`,
			m.session, points, m.session, r, reason, winner, extra))
	}

	fmt.Printf("session %s: %d rallies, %d datagrams sent, %d dropped on purpose\n",
		m.session, *rallies, m.sent, m.dropped)
	return nil
}
