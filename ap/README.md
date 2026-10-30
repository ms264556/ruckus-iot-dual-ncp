# iotd: AP daemon for the dual Zigbee + Bluetooth NCP

`iotd` runs on the AP and turns the dual NCP firmware (Bluetooth NCP on CPC endpoint 2, Zigbee
NCP on CPC endpoint 5, one UART through `cpcd`) into these services:

| Port | Service | Consumers |
|---|---|---|
| **6638** | EZSP in ASH framing, the same protocol a serial EZSP coordinator speaks | Home Assistant ZHA (bellows), zigbee2mqtt (`ember`), other EZSP hosts; limited to `zigbee_allow`, or reached through one outbound stream instead (`zigbee_connect`) |
| **6639** | Bluetooth advertisements, newline-delimited JSON | any client; or streamed out to one address instead (`ble_connect`) |
| **6053** | An ESPHome Bluetooth proxy: the ESPHome native API with Noise encryption, announced over mDNS (`api_port`) | Home Assistant's ESPHome integration, as for an ESPHome device with `api: encryption: key:` |

It also supervises `cpcd` and reconnects when the radio or `cpcd` restarts. It is one static
ARMv7 binary with no dependencies beyond the statically linked `libcpc`.

## Files

| Path | Purpose |
|---|---|
| `iotd/` | the daemon |
| `tools/ncpflash.c` | radio flasher (Gecko bootloader, XMODEM-CRC) |
| `tools/ezsp_probe.c` | direct EZSP check through `cpcd` |
| `start.sh` | stops the vendor IoT gateway and starts `iotd` |
| `cpcd.conf` | `cpcd` configuration for the radio's UART |
| `iotd.conf.example` | `iotd` configuration |
| `test/` | ASH codec unit test and crypto checks (`make test`), BLE stream, ASH and ESPHome API test clients, an API test host |

Build with `scripts/build-ap.sh` from the repository root.

## Configuration

`/writable/data/iotd.conf` (`-c <file>` to use another), `key = value`, `#` comments, all optional:

```
ble = yes                 # serve Bluetooth advertisements
zigbee = yes              # serve EZSP/ASH
ble_port = 6639           # 0: no listener
ble_connect =             # a.b.c.d:port to connect out to and stream to; empty: none
zigbee_port = 6638        # 0: no listener
zigbee_allow =            # IPv4 addresses or networks the listener accepts; empty: anyone
zigbee_connect =          # a.b.c.d:port to connect out to and serve one host on; empty: none
cpcd = /writable/data/bin/cpcd
cpcd_conf = /writable/data/bin/cpcd.conf
```

`ble_connect` makes `iotd` the client: it connects to that address and writes the same stream,
reconnecting 1 s doubling to 30 s whenever the connection fails or closes (the backoff starts over
after a connection that lasted 30 s). With `ble_port = 0` as well, the AP has no Bluetooth
listener. A controller that reaches the AP over an SSH tunnel can point it at a local `-L`
forward, for example `ble_connect = 127.0.0.1:8883`, so the stream travels inside the tunnel.
`ble_port` and `ble_connect` can also both be set.

`zigbee_allow = 192.168.1.10 10.0.0.0/8` (spaces or commas, up to 16 entries) makes the 6638
listener close connections from any other address at once. It is the same idea as the IP allow
lists of network Zigbee coordinators: neither ZHA nor zigbee2mqtt can authenticate to a socket.

`zigbee_connect` is the Zigbee counterpart of `ble_connect`, for a controller that relays the
coordinator to its own port. `iotd` keeps one outbound connection open and idle; the first bytes
that arrive on it (a host's ASH RST) make the other end the Zigbee client, exactly as a new
connection to 6638 would (the NCP is reset for it). When that host's session ends, `iotd` opens a
fresh connection after 1 s. With `zigbee_port = 0` as well, the AP has no Zigbee listener.

Every outbound connection starts with one hello line, `iotd ble 1` or `iotd zigbee 1`, so both
streams can share one destination and the receiving end knows which is which.

The ESPHome Bluetooth proxy is off unless `api_port` is set:

```
api_port = 6053                     # 0: off
api_key = <base64 of 32 bytes>      # the device's encryption key, as ESPHome's api: encryption: key:
api_name = hall-11c8b0              # node name: mDNS instance and host label (lower case, digits, '-')
api_friendly_name = Hall
api_mac = C8:A6:08:11:C8:B0         # the AP's MAC: Home Assistant's unique id for the device
api_model = Ruckus R560
api_bluetooth_mac = F4:B3:B1:E8:0A:CC
api_allow = 192.168.1.10            # IPv4 addresses or networks that may connect; empty: anyone
api_mdns = yes                      # announce _esphomelib._tcp
```

Fixed: `cpcd` instance `cpcd_0`, endpoints 2 and 5, bind address 0.0.0.0, `cpcd` output in
`/tmp/cpcd.log`, and the scan parameters below.

## Running

`start.sh [iotd arguments]` suspends the vendor supervisor `rksiot_gw_base` (SIGSTOP: init would
respawn a killed one, and it reflashes the radio when it runs), kills `rksiot_gw`, `rksiot_ble`,
`rksiot_zb` and any earlier `cpcd` or `iotd`, then starts `iotd` in the background with its log in
`/tmp/iotd.log`. `-v` logs every frame.

`killall iotd` stops it cleanly: clients and endpoints are closed and `cpcd` is terminated.

## Logging

Syslog-style lines on stderr, for example `iotd[30525]: info: zb: NCP reset complete in 302 ms`.
Levels `error`, `warn`, `info`, and `debug` (every CPC, BGAPI and ASH frame in hex) with `-v`. The
Bluetooth module logs its counters every 60 s (advertisements, drops, clients, NCP boots).

## Port 6639: Bluetooth advertisements

One JSON object per line, one line per advertisement report. Input from clients is ignored.

```json
{"ts_ms":123456,"address":"05:32:aa:9f:59:81","address_type":1,"rssi":-50,"channel":37,
 "connectable":true,"scannable":true,"directed":false,"scan_response":false,"extended":false,
 "data":"1eff0600011020..."}
```

| Field | Meaning |
|---|---|
| `ts_ms` | AP monotonic clock in ms when the report was received |
| `address` | advertiser address, lower-case colon hex, most significant byte first |
| `address_type` | BGAPI value: 0 public, 1 random; 255 anonymous (extended reports only) |
| `rssi` | dBm |
| `channel` | advertising channel of the received packet |
| `connectable`, `scannable`, `directed`, `scan_response` | BGAPI event flags 0x1, 0x2, 0x4, 0x8 |
| `extended` | `true` for an extended advertisement report |
| `tx_power`, `data_status` | extended reports only: TX power (127 unknown) and data completeness (0 complete, 1 more follows, 2 truncated) |
| `data` | raw AD bytes in hex: the advertising data, or the scan response when `scan_response` is true |

The `ble_connect` stream carries the same lines. Up to 16 listener clients, plus the outbound one. Each has a 128 KB output buffer; a client that falls behind loses its oldest
records, which are counted, so a slow client never stalls the radio or the other clients.

Scanning: active (scan responses are reported), 1M PHY, observation mode (every advertiser),
interval = window = 100 ms.

## Port 6053: ESPHome Bluetooth proxy

`api.c` serves the ESPHome native API as a passive Bluetooth proxy, the same messages an ESPHome
device with `bluetooth_proxy:` (passive, raw advertisements) answers:

* Transport: `Noise_NNpsk0_25519_ChaChaPoly_SHA256` with the prologue `NoiseAPIInit\0\0`, as in
  ESPHome's `api_frame_helper_noise.cpp`. A client with a different key gets `Handshake MAC failure`
  (Home Assistant: invalid encryption key); a plaintext client gets `Bad indicator byte` (requires
  encryption). There is no plaintext mode.
* Messages: hello, connect, device info (name, MAC, model, manufacturer "Ruckus", friendly name,
  Bluetooth address, feature flags passive scan + raw advertisements), list entities (none), ping,
  get time, disconnect, and the advertisement subscription: raw advertisements (address, RSSI, address
  type, up to 62 bytes of data) in batches of up to 16 every 100 ms. No GATT connections.
* Up to 4 clients; a slow client loses whole batches, never part of a frame.
* `crypto.c` holds SHA-256/HMAC, ChaCha20-Poly1305 (RFC 8439) and X25519 (RFC 7748), checked against
  Python's `cryptography` by `make test`.

`mdns.c` answers queries for `_esphomelib._tcp.local`, the instance (SRV, TXT) and `<api_name>.local`
(A), announces at start and every 5 minutes, and says goodbye on exit. The TXT record carries what
ESPHome devices publish: `friendly_name`, `version`, `mac`, `platform`, `network` and
`api_encryption`, which makes Home Assistant ask for the key. Port 5353 is shared with the
firmware's own `mdnsd` if its Bonjour gateway runs.

## Port 6638: EZSP over ASH

`iotd` is the NCP side of an ASH link (Silicon Labs UG101), so an EZSP host needs nothing beyond
the socket address. EZSP frames are relayed to and from CPC endpoint 5 unchanged.

* `RST` is answered with `RSTACK` (ASH version 2, reset code 0x0B), after the NCP reset described
  below.
* DATA frames carry 3-bit frame and acknowledgement numbers, a data field randomized with the
  ASH pseudo-random sequence, CRC-16/CCITT (big-endian) and byte stuffing of
  `7E 7D 11 13 18 1A`. Host frames are acknowledged at once; NCP frames (responses and callbacks)
  are sent with up to 3 in flight and retransmitted on NAK or after 800 ms. After 6 consecutive
  timeouts the link sends an ERROR frame and waits for `RST`.
* One client at a time; a second connection is closed.

**NCP reset.** A host expects `RST` to reboot the NCP, so that a new session starts with a clean
stack. EZSP has no reset command, so `iotd` reboots the radio by restarting `cpcd`, whose start-up
sequence resets it in about 300 ms. This happens when a client connects, and on any later `RST`
after EZSP traffic. The Bluetooth NCP restarts too: endpoint 2 is reopened and scanning resumes
after a gap of about half a second. The Zigbee network is kept in the radio's NVM3 and survives the
reset, as on a serial coordinator.

When a client disconnects, endpoint 5 is closed. If `cpcd` or the radio restarts while a Zigbee
client is connected, the client's connection is closed and the host reconnects.

## Supervision

* `cpcd` runs as a child of `iotd` and is restarted 1 s after it exits (5 s if it ran under 5 s).
* The `libcpc` connection is retried every 250 ms until `cpcd` accepts it, and rebuilt after a
  radio reset or a lost connection; each endpoint is reopened on the new connection.

## Testing

```bash
make -C ap test                                                   # ASH codec, vectors from bellows
python3 ap/test/ble_stream.py <ap-address> --seconds 30 --quiet   # advertisement summary
python3 ap/test/ash_hold.py <ap-address>                          # RST/RSTACK and connection hold

# The ESPHome API: a host with fake adverts, then the client (Python cryptography only):
make -C ap test/api_harness && ap/test/api_harness 16053 <base64 key> &
python3 ap/test/esphome_client.py 127.0.0.1 --port 16053 --key <base64 key>
python3 ap/test/esphome_client.py <ap-address> --key <base64 key> --seconds 30   # the AP itself
python3 ap/test/esphome_client.py <ap-address> --key <other key> --expect-reject

# EZSP end to end with bellows (pip install bellows):
bellows -d socket://<ap-address>:6638 info            # EUI64, EZSP 13, stack 7.4.0.0
bellows -d socket://<ap-address>:6638 form -D bellows.db
bellows -d socket://<ap-address>:6638 info            # JOINED_NETWORK, COORDINATOR
```

## Limits

* Nothing starts `iotd` at boot; run `start.sh` after a reboot.
* One Zigbee client at a time, as with a serial port.
* The ESPHome proxy is passive: no active scanning control from Home Assistant, no GATT connections.
* Scanning uses the 1M PHY only; Coded PHY (long range) advertisements are not received.
* Logs are in `/tmp` and are lost on reboot.
