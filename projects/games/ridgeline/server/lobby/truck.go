package lobby

import (
	"encoding/binary"
	"math"
)

// Truck is a TRUCK body decoded. The server relays the bytes untouched;
// this is for the bot and the tests (the console packs the same layout in
// src/net.c).
type Truck struct {
	Flags     byte       // TruckSummit, lights (bits 1-2), horn, brakes, stalled
	Pos       [3]float64 // centre of gravity, m
	Quat      [4]float64 // body orientation x, y, z, w (axes right, up, forward)
	Vel       [3]float64 // m/s
	Steer     float64    // front wheels, radians
	RPM, Load float64
	Slip      float64 // the slipperiest tyre, m/s
	Gear      int
	ClockMs   uint32 // the sender's clock when this was current
}

// Truck flag bits beyond TruckSummit.
const (
	TruckLightsShift = 1 // 2 bits: off, dipped, full beam
	TruckHorn        = 1 << 3
	TruckBrake       = 1 << 4
	TruckStalled     = 1 << 5
)

func clampInt(v float64, lo, hi int) int {
	r := int(math.Round(v))
	if r < lo {
		return lo
	}
	if r > hi {
		return hi
	}
	return r
}

func put16(b []byte, v int)  { binary.LittleEndian.PutUint16(b, uint16(int16(v))) }
func get16(b []byte) float64 { return float64(int16(binary.LittleEndian.Uint16(b))) }

// Encode packs the truck into TruckLen bytes.
func (t *Truck) Encode() []byte {
	b := make([]byte, TruckLen)
	b[0] = t.Flags
	for i := 0; i < 3; i++ {
		put16(b[1+i*2:], clampInt(t.Pos[i]*64, -32767, 32767))
		put16(b[15+i*2:], clampInt(t.Vel[i]*256, -32767, 32767))
	}
	for i := 0; i < 4; i++ {
		put16(b[7+i*2:], clampInt(t.Quat[i]*32767, -32767, 32767))
	}
	b[21] = byte(int8(clampInt(t.Steer*160, -127, 127)))
	b[22] = byte(clampInt(t.RPM/32, 0, 255))
	b[23] = byte(clampInt(t.Load*255, 0, 255))
	b[24] = byte(clampInt(t.Slip*16, 0, 255))
	b[25] = byte(int8(clampInt(float64(t.Gear), -1, 9)))
	binary.LittleEndian.PutUint32(b[26:], t.ClockMs)
	return b
}

// DecodeTruck unpacks TruckLen bytes.
func DecodeTruck(b []byte) Truck {
	var t Truck
	t.Flags = b[0]
	for i := 0; i < 3; i++ {
		t.Pos[i] = get16(b[1+i*2:]) / 64
		t.Vel[i] = get16(b[15+i*2:]) / 256
	}
	for i := 0; i < 4; i++ {
		t.Quat[i] = get16(b[7+i*2:]) / 32767
	}
	t.Steer = float64(int8(b[21])) / 160
	t.RPM = float64(b[22]) * 32
	t.Load = float64(b[23]) / 255
	t.Slip = float64(b[24]) / 16
	t.Gear = int(int8(b[25]))
	t.ClockMs = binary.LittleEndian.Uint32(b[26:])
	return t
}

// QuatFromAxes turns body axes (right, up, forward, as world vectors) into
// a quaternion, as the console does.
func QuatFromAxes(r, u, f [3]float64) [4]float64 {
	m00, m01, m02 := r[0], u[0], f[0]
	m10, m11, m12 := r[1], u[1], f[1]
	m20, m21, m22 := r[2], u[2], f[2]
	var x, y, z, w float64
	if tr := m00 + m11 + m22; tr > 0 {
		s := math.Sqrt(tr+1) * 2
		w, x, y, z = s/4, (m21-m12)/s, (m02-m20)/s, (m10-m01)/s
	} else if m00 > m11 && m00 > m22 {
		s := math.Sqrt(1+m00-m11-m22) * 2
		w, x, y, z = (m21-m12)/s, s/4, (m01+m10)/s, (m02+m20)/s
	} else if m11 > m22 {
		s := math.Sqrt(1+m11-m00-m22) * 2
		w, x, y, z = (m02-m20)/s, (m01+m10)/s, s/4, (m12+m21)/s
	} else {
		s := math.Sqrt(1+m22-m00-m11) * 2
		w, x, y, z = (m10-m01)/s, (m02+m20)/s, (m12+m21)/s, s/4
	}
	return [4]float64{x, y, z, w}
}
