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
	// What a person said at the moment of a correction. The tag is from a fixed
	// vocabulary so it groups; the note is free text for the one case it does
	// not cover.
	Tag  string `json:"tag,omitempty"`
	Note string `json:"note,omitempty"`
	// The half is what was measured; the player is that half resolved through
	// the mapping in force at the time, and the name is what it read as then.
	// All three are stored so a mapping that turns out wrong is correctable in
	// the export rather than fatal to the session.
	Player     string `json:"player,omitempty"`
	PlayerName string `json:"player_name,omitempty"`
}

func parseEnvelope(b []byte) (Envelope, error) {
	var e Envelope
	err := json.Unmarshal(b, &e)
	return e, err
}
