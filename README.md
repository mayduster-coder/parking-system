# Parking System

Embedded Linux parking system: an STM32 emulates a GPS device, a BeagleBone Green
forwards the data over Ethernet, and a central TCP server computes parking fees
and stores them in an SQLite database.

## Architecture

```
 +---------+   I2C    +----------------- BeagleBone Green ------------------+   TCP    +-------------------+
 |  STM32  | -------> | process2 (I2C master) --FIFO--> process1 (TCP client)| -------> |  server (Linux PC)|
 | F756ZG  |  0x30    |                    /tmp/gps_fifo                     | Ethernet |  + SQLite DB      |
 +---------+          +------------------------------------------------------+   :8080  +-------------------+
```

| Component | Folder | Language | Role |
|---|---|---|---|
| STM32 GPS emulator | `stm32/` | C (HAL) | I2C slave. Alternates START / END parking messages |
| process2 | `bbg/process2.c` | C | Polls the STM32 over I2C and writes packets to a FIFO |
| process1 | `bbg/process1.c` | C | Reads the FIFO and sends packets to the server over TCP, waits for ACK |
| server | `server/server.c` | C | Multi-client TCP server. Computes fees, stores records, manages prices |
| common packet | `common/common.h` | C | `ParkingData_t`, shared by all components |

## Data flow

1. The STM32 prepares a `ParkingData_t` packet (client ID, START/END, GPS, timestamp).
2. `process2` reads it over I2C (address `0x30`, `/dev/i2c-2`) and writes it to `/tmp/gps_fifo`.
3. `process1` reads the FIFO and sends the packet to the server (`192.168.10.1:8080`).
4. The server replies `0x06` (ACK) or `0x15` (NACK).
5. On **START** the server opens a session. On **END** it computes the fee
   `fee = base_rate + hourly_rate * (seconds / 3600)`
   using the tariff of the city detected from the GPS coordinates.
6. Every packet is stored in the `customer_data` table of `parking.db`.

## Packet format (`ParkingData_t`, 23 bytes, packed)

| Field | Type | Description |
|---|---|---|
| `client_id` | `uint16_t` | Unique client ID |
| `msg_type` | `uint8_t` | 0 = no data, 1 = START, 2 = END |
| `latitude` | `double` | Degrees |
| `longitude` | `double` | Degrees |
| `timestamp` | `uint32_t` | Epoch seconds (STM32 RTC) |

## Price management

- **Console menu** of the server: Add / Update / Remove / Show city prices.
- **File + signal**: write lines `city,hourly,base` to `prices.txt`, then
  `kill -USR1 <server_pid>`. The server reloads the prices into the database.

## Build and run

**Server (PC / VM):**
```bash
sudo apt install libsqlite3-dev
cd server && make && ./server
```

**BeagleBone:**
```bash
cd bbg && make
sudo ./process2      # terminal 1
sudo ./process1      # terminal 2
```

**STM32:** open `stm32/` in STM32CubeIDE, build and flash.

## Network setup

| Host | Interface | IP |
|---|---|---|
| Server VM | `enp0s8` (VirtualBox bridged to the PC Ethernet port) | `192.168.10.1/24` |
| BeagleBone | `eth0` | `192.168.10.2/24` |

## Logs

Each component writes its own timestamped log: `process1.log`, `process2.log`, `server.log`.

## Documentation

Generate the API documentation with Doxygen:
```bash
sudo apt install doxygen
doxygen Doxyfile
xdg-open docs/html/index.html
```
