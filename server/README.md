# Def Jam Android room server

This UDP room server adapts [FFNY Online by sweeax-exe and contributors](https://github.com/sweeax-exe/ffny-online/tree/5664adca2c91364e82dc11dab08f81776ed90d91). See [LICENSE](LICENSE) for the original MIT notices. The Android port and its own-server extensions are maintained by contatg.

From this directory, with Go 1.22 or newer:

```sh
go test ./...
go build -o defjam-server ./cmd/defjam-server
./defjam-server -listen :27777
```

On Windows, build with `go build -o defjam-server.exe ./cmd/defjam-server` and run `./defjam-server.exe -listen :27777` in PowerShell.

In the Android app's online settings, select your own server. Both players enter the server host's reachable IPv4 address followed by `:27777`. A LAN or a ZeroTier network can carry the UDP traffic. Room state lives in memory and ends when the process exits. Private rooms in this server can require a password. See the [project credits](../CREDITS.md) for attribution.
