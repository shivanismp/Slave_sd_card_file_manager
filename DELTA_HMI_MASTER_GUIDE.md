# Delta HMI Slave / ESP32-C6 Master Integration

## What changed

Only the physical UART0 role changed.

```text
MQTT binary Modbus request
        |
        v
modbus_slave_process_frame()
        |
        v
Existing software register banks and broker
```

The MQTT parser remains in `modbus_slave.c` and still uses Modbus slave ID 95.
No `mqtt_modbus_server.c` split or function rename is required.

```text
ESP32-C6 UART0 Modbus master
        |
        v
Delta HMI Modbus RTU slave, station 95
```

The historical `app_modbus_slave()` function name is retained, but it now starts the Delta HMI master task.
`main.c` and CMake integration remain structurally unchanged.

## Preserved MQTT dependency

This line is still active:

```c
mqtt_modbus_slave.modbus_slave_process_frame = modbus_slave_process_frame;
```

The following existing items are also preserved:

- `slave_holding_regs[]`
- `slave_input_regs[]`
- `modbus_slave_process_frame()`
- `modbus_slave_sync_from_runtime()`
- `modbus_slave_apply_pending_writes()`
- existing write broker and downstream dsPIC write functions

MQTT and Delta HMI therefore use the same runtime/register image.

## Delta communication settings

Configure the connected Delta COM port as:

```text
Protocol           Modbus RTU Slave
HMI station        95
PLC station        Leave default
Baud rate          9600
Data bits          8
Parity             None
Stop bits          1
Interface          RS-485 2-wire
```

Firmware pins remain:

```text
UART0 TX   GPIO20
UART0 RX   GPIO6
RS-485 DIR GPIO7
```

## Disable the previous Data Exchange table

The ESP32 now performs the data exchange itself. Disable the old timed Data Exchange rows.

Screen objects should reference Delta internal `$` registers directly, not addresses such as:

```text
{RS485_serial_2}95@R-0
```

## Showing values in Delta

The ESP32 writes runtime feedback from the existing `slave_input_regs[]` image.

### Fast numeric values

| Delta register | Existing input register | Meaning |
|---:|---:|---|
| `$0` | 0 | Machine state feedback |
| `$1` | 1 | Auto-power feedback |
| `$2` | 2 | Actual power percentage |
| `$3` | 3 | Line 1 voltage |
| `$4` | 4 | Line 1 current |
| `$5` | 5 | Line 2 voltage |
| `$6` | 6 | Line 2 current |
| `$7` | 7 | Line 3 voltage |
| `$8` | 8 | Line 3 current |
| `$9` | 9 | Average voltage |
| `$10` | 10 | Average current |
| `$11` | 11 | PWM frequency |
| `$12` | 12 | Average kW |
| `$13` | 13 | Average PF |
| `$14` | 14 | Error bit field |
| `$15` | 15 | Melter temperature |
| `$18` | 18 | Chiller temperature |
| `$19` | 19 | IGBT plate temperature |
| `$20` | 20 | Coil temperature |
| `$21` | 21 | Communication status |
| `$22` | 22 | Local set temperature |
| `$23` | 23 | Local auto-power value |

For a numeric display, select an internal HMI register and use the matching `$` address.

### Error lamps

Use `$14` as the word address and select its individual bits:

| Bit | Error |
|---:|---|
| 0 | LM error |
| 1 | PWM trip |
| 2 | WF error |
| 3 | Chiller overheat |
| 4 | IGBT overheat |
| 5 | External input 1 |
| 6 | External input 2 |
| 7 | Temperature cutoff |
| 8 | HF PT trip |
| 9 | HF CT trip |
| 10 | Phase error |

### Information and text registers

| Delta range | Content |
|---:|---|
| `$42-$61` | Machine model |
| `$62-$81` | Machine capacity |
| `$82-$101` | IoT ID |
| `$102-$121` | Firmware version |
| `$122-$141` | HMI heading text |
| `$142` | Network connectivity state (`0` disconnected, `4` Ethernet online) |
| `$143` | Hour |
| `$144` | Minute |
| `$145` | AM/PM: 0=N/A, 1=AM, 2=PM |
| `$146` | Weekday |
| `$147` | Date |
| `$148` | Month |
| `$149-$168` | AP SSID |
| `$169-$188` | AP password |

These strings use one ASCII character per 16-bit register, matching the existing firmware representation.

## Setting values from Delta

The ESP32 reads operator commands from Delta `$300-$312` and maps them to
holding-register indices `0-12`. Display/bar ranges at `$313-$338` are owned
by the ESP32 and written to the HMI; they are not staged as operator commands.

| Delta register | Existing holding index | Meaning |
|---:|---:|---|
| `$300` | 0 | Machine trigger |
| `$301` | 1 | Control mode |
| `$302` | 2 | Minimum frequency |
| `$303` | 3 | Maximum frequency |
| `$304` | 4 | Minimum POT calibration |
| `$305` | 5 | Maximum POT calibration |
| `$306` | 6 | Minimum CT calibration |
| `$307` | 7 | Maximum CT calibration |
| `$308` | 8 | Minimum PT calibration |
| `$309` | 9 | Maximum PT calibration |
| `$310` | 10 | Auto-power percentage |
| `$311` | 11 | Melter set temperature |
| `$312` | 12 | PID auto-tune command |

### ON and OFF buttons

Create Set Value buttons:

```text
ON button:  write 2 to $300
OFF button: write 1 to $300
```

The project enum is:

```text
0 IDLE
1 OFF
2 ON
3 TEMP_CUT_OFF
4 TEMP_CUT_ON
5 ERROR
```

### Auto-power numeric entry

```text
Address  $310
Minimum  0
Maximum  100
```

### Frequency entries

```text
Minimum frequency $302
Maximum frequency $303
```

The master reads both values as one command block and stages the frequency pair together.

## MQTT and HMI synchronization

- A Delta change is placed into the same existing broker used by MQTT FC06/FC16 writes.
- `modbus_slave_apply_pending_writes()` still sends those commands to the control/sensor cards.
- MQTT-originated or runtime-originated changes are mirrored back to `$300-$312`.
- A short pending period prevents an HMI command from being immediately overwritten before the downstream card confirms it.

## UART0 console warning

The project currently configures UART0 as the ESP console and also uses UART0 for RS-485. Any `printf` or enabled ESP log sent through UART0 can corrupt Modbus frames.

Use one of these approaches:

1. Configure the primary console as USB Serial/JTAG and disable the UART0 console, or
2. Keep all UART console output disabled after the RS-485 master starts.

The corrected project does not change this unrelated SDK setting automatically.
