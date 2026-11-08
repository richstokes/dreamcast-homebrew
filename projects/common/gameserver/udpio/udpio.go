// Package udpio is the UDP socket of the game servers (DreamKarts,
// Ridgeline): datagrams in on a channel for the server's one goroutine to
// handle, replies out, and an optional pretend WAN for testing consoles on
// a LAN.
package udpio

import (
	"log"
	"math/rand"
	"net"
	"net/netip"
	"time"
)

// MaxPacket is the largest datagram read: anything longer is dropped
// unread, as no console sends one.
const MaxPacket = 512

// Packet is one datagram received.
type Packet struct {
	From netip.AddrPort
	Data []byte
}

// Conn is a listening socket.
type Conn struct {
	conn  *net.UDPConn
	in    chan Packet
	delay func() time.Duration
}

// Listen opens addr (":7535"). With lag or jitter, every packet each way
// waits half the lag plus a random share of the jitter, as on a WAN.
func Listen(addr string, lag, jitter time.Duration) (*Conn, error) {
	a, err := net.ResolveUDPAddr("udp", addr)
	if err != nil {
		return nil, err
	}
	conn, err := net.ListenUDP("udp", a)
	if err != nil {
		return nil, err
	}
	c := &Conn{conn: conn, in: make(chan Packet, 1024), delay: func() time.Duration { return 0 }}
	if lag > 0 || jitter > 0 {
		log.Printf("testing with %v lag and %v jitter", lag, jitter)
		c.delay = func() time.Duration {
			d := lag / 2
			if jitter > 0 {
				d += time.Duration(rand.Int63n(int64(jitter)))
			}
			return d
		}
	}
	go c.read()
	return c, nil
}

// LocalAddr is where the socket listens.
func (c *Conn) LocalAddr() net.Addr { return c.conn.LocalAddr() }

// In delivers the datagrams received; it closes if the socket fails.
func (c *Conn) In() <-chan Packet { return c.in }

func (c *Conn) queue(p Packet) {
	select {
	case c.in <- p:
	default: // overloaded: drop rather than queue stale state
	}
}

func (c *Conn) read() {
	buf := make([]byte, 1500)
	for {
		n, from, err := c.conn.ReadFromUDPAddrPort(buf)
		if err != nil {
			if ne, ok := err.(net.Error); ok && ne.Timeout() {
				continue
			}
			log.Printf("read: %v", err)
			close(c.in)
			return
		}
		if n > MaxPacket {
			continue
		}
		p := Packet{from, append([]byte(nil), buf[:n]...)}
		if d := c.delay(); d > 0 {
			time.AfterFunc(d, func() { c.queue(p) })
		} else {
			c.queue(p)
		}
	}
}

// Send writes b to a console (after the pretend lag, if any). Safe to
// reuse b once it returns.
func (c *Conn) Send(to netip.AddrPort, b []byte) {
	if d := c.delay(); d > 0 {
		b := append([]byte(nil), b...)
		time.AfterFunc(d, func() { c.write(to, b) })
		return
	}
	c.write(to, b)
}

func (c *Conn) write(to netip.AddrPort, b []byte) {
	if _, err := c.conn.WriteToUDPAddrPort(b, to); err != nil {
		log.Printf("write %v: %v", to, err)
	}
}
