// Command log-sink receives the firmware's diagnostic events over UDP, joins
// the chunks a large event was split into, notices what never arrived, and
// writes one append-only file per session.
//
// It also serves a small viewer, because a curve is the one thing a JSONL file
// cannot show you.
//
//	log-sink                       listen on :9000, viewer on :9001
//	log-sink -dir ./data           somewhere else to write
//	log-sink export <file.jsonl>   a zip with the file and a summary
//	log-sink games <file.jsonl>    one readable JSON per game, curves left out
//	log-sink mock                  send a made-up session at it, no board needed
package main

import (
	"flag"
	"fmt"
	"log"
	"net"
	"os"
	"os/signal"
	"syscall"
	"time"
)

func main() {
	log.SetFlags(log.Ltime)

	if len(os.Args) > 1 {
		switch os.Args[1] {
		case "export":
			if err := runExport(os.Args[2:]); err != nil {
				log.Fatal(err)
			}
			return
		case "games":
			if err := runGames(os.Args[2:]); err != nil {
				log.Fatal(err)
			}
			return
		case "mock":
			if err := runMock(os.Args[2:]); err != nil {
				log.Fatal(err)
			}
			return
		}
	}

	var (
		addr     = flag.String("listen", ":9000", "UDP address to receive on")
		web      = flag.String("viewer", ":9001", "HTTP address for the viewer, empty to switch it off")
		dir      = flag.String("dir", ".", "where to write sessions/")
		patience = flag.Duration("patience", 3*time.Second,
			"how long an event may wait for missing chunks before it is written off")
	)
	flag.Parse()

	sink, err := NewSink(*dir, *patience)
	if err != nil {
		log.Fatal(err)
	}
	defer sink.Close()

	pc, err := net.ListenPacket("udp", *addr)
	if err != nil {
		log.Fatal(err)
	}
	defer pc.Close()

	// A generous receive buffer. The firmware sends a chunked hit as a burst of
	// datagrams back to back, and a small buffer turns a burst into loss that
	// looks like the radio's fault.
	if uc, ok := pc.(*net.UDPConn); ok {
		if err := uc.SetReadBuffer(1 << 20); err != nil {
			log.Printf("could not enlarge the receive buffer: %v", err)
		}
	}

	// Named as they actually are: everything a day produced under that day, with
	// the finished games beside the raw sessions.
	// The board asks every couple of seconds whether anybody is there; this is
	// the yes. Without it the board holds everything it produces.
	sink.Answer = func(to net.Addr) {
		pc.WriteTo([]byte(`{"v":1,"type":"pong"}`), to)
	}

	log.Printf("listening on %s", *addr)
	log.Printf("writing to %s/<day>/sessions and %s/<day>/games", *dir, *dir)
	if *web != "" {
		go serveViewer(*web, *dir, sink)
		log.Printf("viewer on http://localhost%s", *web)
	}

	stop := make(chan os.Signal, 1)
	signal.Notify(stop, os.Interrupt, syscall.SIGTERM)

	go func() {
		t := time.NewTicker(time.Second)
		defer t.Stop()
		for range t.C {
			sink.Sweep(time.Now())
		}
	}()

	go func() {
		buf := make([]byte, 65535)
		for {
			n, from, err := pc.ReadFrom(buf)
			if err != nil {
				return
			}
			raw := make([]byte, n)
			copy(raw, buf[:n])
			sink.Handle(raw, from, time.Now())
		}
	}()

	<-stop
	fmt.Println()
	// On the way out nothing is going to arrive any more, so patience is over:
	// an event still waiting for chunks has to be written off now or it
	// disappears without a word, which is the silent loss this is against.
	sink.Sweep(time.Now().Add(time.Hour))
	for _, se := range sink.Sessions() {
		fmt.Print(se.Summary().Text())
	}
}
