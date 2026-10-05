# Modbus implementation and compliance matrices

Two matrices are kept here so that "where is this requirement implemented?" can
be answered without reading the whole source tree, and so that no function code
is called implemented without both a positive and a negative test.

Column definitions:

- **spec** — section of MODBUS Application Protocol Specification V1.1b3, or of
  MODBUS over Serial Line Specification V1.02 where marked `ser`.
- **file:function** — the implementation that owns the behaviour.
- **test** — the check in this directory that exercises it, by name.

---

## 1. Data representation and framing

| Requirement | spec | file:function | test |
|---|---|---|---|
| PDU is function code plus data | app 4.1 | `modbus_pdu.c:modbus_pdu_parse` | protocol: function codes are rejected when truncated |
| PDU is at most 253 bytes | app 4.1 | `modbus_pdu.c:modbus_pdu_parse` | protocol: "a request longer than the PDU limit is refused" |
| Function code 0 invalid, 0x80.. reserved | app 4.1 | `modbus_pdu.c:modbus_pdu_parse` | protocol: "function code 0 is not a valid PDU", "a response code in a request is not parsed as a PDU" |
| Quantities are most significant byte first | app 4.2 | `modbus_pdu.h:modbus_get_u16` | protocol: "FC 03 returns the value most significant byte first" |
| Addressed item X is data X−1 | app 4.4 | `modbus_fc.c` handlers | protocol: read after write at a known address |
| No silent address wrap | app 4.4 | `modbus_backend.c:modbus_backend_range` | protocol: "FC 01 rejects a range that crosses the end of the area" |
| Unit 0 broadcast, 1..247 valid, 248..255 reserved | ser 2.2 | `ng_modbus.c:ng_modbus_check_config` | none yet; see gaps |
| RTU frame: address, FC, data, CRC | ser 2.5.1.1 | `modbus_rtu.c:modbus_rtu_input` | framing: "RTU completes a frame when the CRC lands" |
| t3.5 inter-frame silence | ser 2.5.1.1 | `modbus_rtu.c:modbus_rtu_t35_ns` | framing: "t3.5 at 9600 Bd is 3.5 character times" |
| t1.5 inter-character silence discards | ser 2.5.1.1 | `modbus_rtu.c:modbus_rtu_input` | framing: "RTU discards a frame abandoned by a long silence" |
| Fixed timers above 19200 Bd | ser 2.5.1.1 | `modbus_rtu.c:modbus_rtu_t15_ns` | framing: "t1.5 above 19200 Bd is 750 us" |
| CRC-16, init FFFF, poly A001, low byte first | ser 2.5.1.2, app B | `modbus_crc.c:modbus_crc16` | framing: "CRC of the appendix B example is 0x1241" |
| ASCII: colon, hex pairs, LRC, CR LF | ser 2.5.2 | `modbus_ascii.c:modbus_ascii_input` | framing: "ASCII decoder returns one frame" |
| LRC is the two's complement of the byte sum | ser 2.5.2.2 | `modbus_ascii.c:modbus_lrc` | framing: "LRC of a read holding registers request" |
| Odd hex digit count is malformed | ser 2.5.2.1 | `modbus_ascii.c:modbus_ascii_input` | framing: "ASCII refuses an odd digit count" |
| A second colon abandons the frame | ser 2.5.2.1 | `modbus_ascii.c:modbus_ascii_input` | framing: "a colon restarts the ASCII frame" |
| MBAP is TID, PID, LEN, UID | tcp 3.1 | `modbus_tcp.c:modbus_tcp_input` | framing: "MBAP plus a five byte PDU is twelve bytes" |
| PID must be zero | tcp 3.1 | `modbus_tcp.c:modbus_tcp_check_header` | framing: "TCP rejects a non zero protocol identifier" |
| LEN counts UID plus PDU | tcp 3.1.3 | `modbus_tcp.c:modbus_tcp_check_header` | framing: "the length field counts the unit identifier and the PDU" |
| TCP is a byte stream | tcp 3.1 | `modbus_tcp.c:modbus_tcp_input` | framing: byte-at-a-time reassembly and two coalesced requests |

---

## 2. Function code matrix

`req` is the request data length in bytes, excluding the function code.
Limits are those of the request diagrams.

| FC | spec | req | address range | quantity | response | exceptions covered | backend op | broadcast | tests |
|---|---|---|---|---|---|---|---|---|---|
| 01 Read Coils | 6.1 | 4 | must fit area | 1..2000 | FC, byte count, packed bits | 02 out of map, 03 qty 0 / qty 2001 / truncated | read bits | no | positive: "FC 01 packs the lowest bit…"; negative: qty 0, qty 2001, address past end, range across end |
| 02 Read Discrete Inputs | 6.2 | 4 | must fit area | 1..2000 | FC, byte count, packed bits | 02, 03 as 01 | read bits (discrete) | no | "FC 02 reads a discrete input", "FC 02 reads a separate area" |
| 03 Read Holding Registers | 6.3 | 4 | must fit area | 1..125 | FC, byte count, registers | 02 out of map, 03 qty 0 / qty 126 / truncated | read registers | no | positive: "FC 03 returns the registers written by FC 10"; negative: qty 0, qty 126, range across end |
| 04 Read Input Registers | 6.4 | 4 | must fit area | 1..125 | FC, byte count, registers | 02, 03 as 03 | read registers (input) | no | "FC 04 reads an input register", "FC 04 does not read the holding register area" |
| 05 Write Single Coil | 6.5 | 4 | must fit area | 1 | echoed request | 02, 03 bad value | write bits | yes | positive: "FC 05 echoes the request"; negative: "FC 05 rejects a value that is not 0x0000 or 0xff00" |
| 06 Write Single Register | 6.6 | 4 | must fit area | 1 | echoed request | 02, 03 truncated | write registers | yes | positive: "FC 06 echoes the request" |
| 15 Write Multiple Coils | 6.11 | 5+N/8 | must fit area | 1..2000 | FC, address, quantity | 02, 03 byte count mismatch, qty 0 | write bits | yes | positive: "FC 0F accepts 9 coils"; negative: byte count mismatch (shared code path with FC 10) |
| 16 Write Multiple Registers | 6.12 | 5+2N | must fit area | 1..123 | FC, address, quantity | 02, 03 byte count mismatch, qty 0, qty 124 | write registers | yes | positive: "FC 10 echoes the address and quantity"; negative: "FC 10 rejects a byte count that is not twice the quantity", "FC 10 rejects quantity 124", "FC 10 rejects a range that crosses the end of the area" |
| 22 Mask Write Register | 6.16 | 6 | must fit area | 1 | echoed request | 02, 03 wrong length | mask register | yes | positive: "FC 16 applies a mask write", "FC 16 computes (current AND and) OR (or AND NOT and)"; negative: "FC 16 rejects a request that is not 6 bytes of data" |
| 23 Read/Write Multiple Registers | 6.17 | 9+2W | must fit area | read 1..125, write 1..121 | FC, byte count, read registers | 02, 03 read qty 126, write qty 122, byte count mismatch | write then read registers | yes | positive: "FC 17 returns only the read data", "FC 17 performed the write before the read"; negative: write qty 122, read qty 126 |
| other | 7 | — | — | — | exception 01 | 01 | — | — | "an unimplemented function code answers exception 01", "function code 07 is not implemented" |

---

## 3. Exception mapping

| Code | Name | Produced where | Test |
|---|---|---|---|
| 01 | ILLEGAL FUNCTION | `modbus_fc.c` default case | yes |
| 02 | ILLEGAL DATA ADDRESS | `modbus_backend.c` range and area checks | yes |
| 03 | ILLEGAL DATA VALUE | every handler's validation | yes |
| 04 | SERVER DEVICE FAILURE | defined, not produced: the memory backend has no unrecoverable failure | n/a |
| 05..0A | ACKNOWLEDGE, DEVICE BUSY, NEGATIVE ACKNOWLEDGE, MEMORY PARITY, GATEWAY | defined for ABI stability, never produced by this implementation | n/a |

---

## 4. Known gaps

| Gap | Why | Where it will be covered |
|---|---|---|
| Broadcast is not answered | unit identifiers only exist on framed transports; the rule is implemented in the node paths but not covered by a host test | kyua test, and RTU on hardware |
| Request for another unit is ignored | implemented in `ng_modbus_input_tcp` and the serial paths, not host tested | kyua test |
| Client role does nothing yet | no transaction state, no timeout | Milestone 4 remainder |
| RTU timing uses a synthetic timestamp in the node | a real deadline needs a callout armed on the first byte of a frame | timer work, then hardware |
| No kyua/ATF tests | needs a FreeBSD system to run them | `tests/sys/netgraph/` |
| No interoperability run against another implementation | needs a host with FreeBSD | Milestone 8 |
| `getinfo` advertises no framing | conservative: `transports` is 0 until a framer has a real timer or peer | per framer, with its test |