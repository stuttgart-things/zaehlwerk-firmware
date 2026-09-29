package main

import "encoding/json"

// Envelope is the head every event from the firmware carries. The rest of the
// fields differ per type and are kept as raw JSON: the sink stores and counts,
// it does not need to understand a hit in full, and a schema that grows a field
// should not need a change here.
type Envelope struct {
	V         int    `json:"v"`
	SessionID string `json:"session_id"`
	Seq       uint64 `json:"seq"`
	ID        string `json:"id"`
	TUs       uint64 `json:"t_us"`
	Type      string `json:"type"`

	// Present only on a chunk. A chunk is not an event: it carries a slice of
	// one event's JSON as a string, and the parts are joined before parsing.
	Chunk *Chunk `json:"chunk,omitempty"`
	Part  string `json:"part,omitempty"`
}

type Chunk struct {
	I int `json:"i"`
	N int `json:"n"`
}

// Hit is the part of a hit event the summary counts. Everything else stays in
// the stored record.
type Hit struct {
	Decision string  `json:"decision"`
	Counted  bool    `json:"counted"`
	Side     *string `json:"side"`
	Ratio    float64 `json:"ratio"`
	PeakA    int     `json:"peak_a"`
	PeakB    int     `json:"peak_b"`
	Intended *struct {
		Side string `json:"side"`
		Type string `json:"type"`
	} `json:"intended,omitempty"`
}

// Point is the part of a point event the summary counts.
type Point struct {
	PointID string `json:"point_id"`
	RallyID string `json:"rally_id"`
	Reason  string `json:"reason"`
	Side    string `json:"side"`
}

func parseEnvelope(b []byte) (Envelope, error) {
	var e Envelope
	err := json.Unmarshal(b, &e)
	return e, err
}
