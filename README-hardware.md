# CMRInet hardware

Part of the [CMRInet](README.md) guide set.

## cpNode-Xiao

- Board details [on SPCoast.com](https://www.spcoast.com/versions/cpnode-xiao/)
- Design files [on github.com](https://github.com/plocher/cpNode-Xiao)

The cpNode-Xiao is a SPCoast/MRCS product that combines a Seeed Xiao
module with a CMRI RS-422 4-wire bus interface, an OLED display, and a
5 V I2C bus that connects to I2C IO expanders.

Pinout:
```
   D7 - GPIO17  RX      CMRI bus receive (RS-422/RS-485)
   D6 - GPIO16  TX      CMRI bus transmit (RS-422/RS-485)
   D5 - GPIO23  SCL     I2C
   D4 - GPIO22  SDA     I2C
   D3 - GPIO21  TXEN    RS-422/RS-485 transmit enable
   D2 - GPIO02          cpNode-Xiao onboard pushbutton input (active-low)
   na - GPIO15  LED_BUILTIN (not brought out to a XIAO pin)
```

## cpNode-IOX

- Board details: [on SPCoast.com](https://www.spcoast.com/versions/cpnode-iox/)
- Design files: [on github.com](https://github.com/plocher/cpNode-IOX)

The cpNode-IOX is a follow-on to MRCS' IOX series of expanders. It
provides:

- local 5 V, 750 mA regulated power for attached accessories
- 0.100 inch pitch screw terminals for layout connections
- I2C address selection and display
- daisy-chained 7.5 to 12 V DC unregulated power source feed-through

Generic MCP23017 breakout boards work too. Each expander contributes
16 bits in two 8-bit ports, and each port is all input or all output.

## Bus wiring

The bus runs at 28800 baud, 8N2, over either topology.

- 4-wire RS-422 topology
  - Host's T± to the Node's R±, and Host's R± to the Node's T±. This is
    a crossover cable.
  - All the Nodes on the bus are wired with a straight-through cable:
    their T± pairs and R± pairs daisy-chained to each other, plus to
    plus, minus to minus.
- 2-wire RS-485 topology
  - All the devices (Host and Node) are wired straight through with
    their A+/B- pair daisy-chained to each other, plus to plus, minus
    to minus.
- Hybrid: 4-wire devices on a 2-wire bus
  - Many boards in the CMRI ecosystem have a 5-pin bus connector: a T±
    pair, an R± pair, and shield.
  - You can use them with a 2-wire device by:
    - connecting all the 4-wire devices as "4-wire" above;
    - looping back the T± pair to the R± pair at one end of the 4-wire
      segment;
    - connecting the A+/B- RS-485 pair to either the T± pair or the R±
      pair. The loopback joins them, so there is no difference between
      them anymore.
  - This ONLY works with CMRI Host implementations and RS-485 interface
    hardware that supports transmit enable (TXEN). CMRInet and the
    cpNode-Xiao fully support TXEN in a full-duplex RS-485 environment.

Which name? RS-422 is the 4-wire, full-duplex system; RS-485 is the
2-wire, half-duplex system. The cpNode-Xiao hardware supports both.
Most users won't care, and continue to use the original 4-wire cabling
no matter what it is called.
