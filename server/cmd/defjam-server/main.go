// Contatg Def Jam Online. This process, not GitHub Pages, owns live rooms.
package main

import (
	"context"
	"defjam/netplay/relay"
	"flag"
	"log"
	"net"
	"os"
	"os/signal"
	"syscall"
)

func main() {
	listen := flag.String("listen", ":27777", "UDP listen address")
	maxRooms := flag.Int("max-rooms", 256, "maximum live rooms")
	maxClients := flag.Int("max-clients", 512, "maximum connected players")
	flag.Parse()
	if *maxRooms < 1 || *maxClients < 2 {
		log.Fatal("max-rooms must be >=1 and max-clients >=2")
	}
	addr, e := net.ResolveUDPAddr("udp", *listen)
	if e != nil {
		log.Fatal(e)
	}
	conn, e := net.ListenUDP("udp", addr)
	if e != nil {
		log.Fatal(e)
	}
	server, e := relay.NewServer(relay.ServerConfig{Relay: relay.Config{MaxRooms: *maxRooms, MaxClients: *maxClients, RequirePrivatePasswords: true, MsgsPerSecond: 2000}})
	if e != nil {
		log.Fatal(e)
	}
	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer stop()
	log.Printf("Contatg Def Jam Online: UDP %s; public rooms + password-protected private rooms", conn.LocalAddr())
	log.Print("GitHub may publish the relay address in online.json; it does not run this process.")
	if e = server.Serve(ctx, conn); e != nil {
		log.Fatal(e)
	}
}
