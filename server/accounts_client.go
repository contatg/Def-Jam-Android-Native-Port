package relay

import (
	"bytes"
	"context"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net/http"
	"strings"
)

// Accounts is the account service as the relay needs it for ranked play.
type Accounts interface {
	// Redeem exchanges a one-time ticket for the account behind it; ErrTicketRefused when the
	// ticket is unknown, used or expired.
	Redeem(ctx context.Context, ticket [TicketLen]byte) (RankedPlayer, error)
	// Paired records a ranked match the relay started.
	Paired(ctx context.Context, p RankedPairing) error
}

// ErrTicketRefused is the account service's "no such ticket".
var ErrTicketRefused = errors.New("ranked ticket refused")

// AccountsHTTP talks to the account service's local internal listener (never the public one).
type AccountsHTTP struct {
	Base   string // e.g. "http://127.0.0.1:8091"
	Client *http.Client
}

func (a AccountsHTTP) post(ctx context.Context, path string, in, out any) (int, error) {
	body, err := json.Marshal(in)
	if err != nil {
		return 0, err
	}
	req, err := http.NewRequestWithContext(ctx, http.MethodPost, strings.TrimRight(a.Base, "/")+path, bytes.NewReader(body))
	if err != nil {
		return 0, err
	}
	req.Header.Set("Content-Type", "application/json")
	client := a.Client
	if client == nil {
		client = http.DefaultClient
	}
	res, err := client.Do(req)
	if err != nil {
		return 0, err
	}
	defer res.Body.Close()
	if res.StatusCode/100 != 2 {
		io.Copy(io.Discard, io.LimitReader(res.Body, 4<<10))
		return res.StatusCode, fmt.Errorf("%s: HTTP %d", path, res.StatusCode)
	}
	if out == nil {
		return res.StatusCode, nil
	}
	return res.StatusCode, json.NewDecoder(io.LimitReader(res.Body, 64<<10)).Decode(out)
}

func (a AccountsHTTP) Redeem(ctx context.Context, ticket [TicketLen]byte) (RankedPlayer, error) {
	var r struct {
		Account   int64   `json:"account"`
		Name      string  `json:"name"`
		Rating    float64 `json:"rating"`
		Deviation float64 `json:"deviation"`
	}
	code, err := a.post(ctx, "/internal/ranked/redeem", map[string]string{"ticket": hex.EncodeToString(ticket[:])}, &r)
	if code == http.StatusNotFound {
		return RankedPlayer{}, ErrTicketRefused
	}
	if err != nil {
		return RankedPlayer{}, err
	}
	if r.Account <= 0 {
		return RankedPlayer{}, ErrTicketRefused
	}
	return RankedPlayer{Account: r.Account, Name: r.Name, Rating: r.Rating, Deviation: r.Deviation}, nil
}

func (a AccountsHTTP) Paired(ctx context.Context, p RankedPairing) error {
	type seat struct {
		Slot    int   `json:"slot"`
		Account int64 `json:"account"`
	}
	in := struct {
		Key   string `json:"key"`
		Seats []seat `json:"seats"`
	}{Key: PairingKey(p.Room, p.Seed)}
	for _, s := range p.Seats {
		in.Seats = append(in.Seats, seat{int(s.Slot), s.Account})
	}
	_, err := a.post(ctx, "/internal/ranked/pairing", in, nil)
	return err
}

// PairingKey is how the games name the matches of one START in their reports: the room code
// and the seed in upper-case hex ("ABC123-1A2B3C4D"), then "-<match number>".
func PairingKey(room string, seed uint32) string { return fmt.Sprintf("%.6s-%08X", room, seed) }

