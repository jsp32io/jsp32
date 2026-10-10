package main

import (
	"bytes"
	"fmt"
	"io"
	"os"
	"sync"

	"github.com/xtaci/smux"
)

// No network: stdin/stdout form the reliable, ordered duplex transport.
type transport struct {
	done chan struct{}
	once sync.Once
}

func (t *transport) Read(b []byte) (int, error) {
	n, err := os.Stdin.Read(b)
	if err != nil {
		t.once.Do(func() { close(t.done) })
	}
	return n, err
}
func (*transport) Write(b []byte) (int, error) { return os.Stdout.Write(b) }
func (*transport) Close() error                { return nil }

func main() {
	config := smux.DefaultConfig()
	config.KeepAliveDisabled = true
	var session *smux.Session
	var err error
	isClient := os.Args[1] == "client"
	conn := &transport{done: make(chan struct{})}
	if isClient {
		session, err = smux.Client(conn, config)
	} else {
		session, err = smux.Server(conn, config)
	}
	if err != nil {
		panic(err)
	}
	defer session.Close()
	var wg sync.WaitGroup
	for i := 0; i < 3; i++ {
		var stream *smux.Stream
		if isClient {
			stream, err = session.OpenStream()
		} else {
			stream, err = session.AcceptStream()
		}
		if err != nil {
			panic(err)
		}
		wg.Add(1)
		go func(index int, stream *smux.Stream) {
			defer wg.Done()
			defer stream.Close()
			payload := make([]byte, 100003)
			for j := range payload {
				payload[j] = byte(j*13 + index)
			}
			if isClient {
				if _, err := stream.Write(payload); err != nil {
					panic(err)
				}
				reply, err := io.ReadAll(stream)
				if err != nil {
					panic(err)
				}
				if !bytes.Equal(payload, reply) {
					panic("echo mismatch")
				}
				fmt.Fprintln(os.Stderr, "received echo", index)
			} else {
				request := make([]byte, len(payload))
				if _, err := io.ReadFull(stream, request); err != nil {
					panic(err)
				}
				if !bytes.Equal(payload, request) {
					panic("request mismatch")
				}
				if _, err := stream.Write(request); err != nil {
					panic(err)
				}
			}
		}(i, stream)
	}
	wg.Wait()
	// Keep stdout alive until JS has sent its final FIN and closed its transport.
	<-conn.done
	fmt.Fprintln(os.Stderr, "interop OK")
}
