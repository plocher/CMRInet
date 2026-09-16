To connect JMRI to the XiaoNode-IP board over TCP, use the following settings in JMRI:

1. Connection Settings
Open Preferences → Connections (or PanelPro → Preferences → Connections):

-  System Manufacturer: C/MRI
-  System Connection: Network Interface
-  Settings:
   - IP Address / Host Name: Enter the board's WiFi IP address (displayed on the bottom line of the OLED display, or in the Arduino Serial Monitor at startup, e.g., 192.168.1.150).
   - Port: 2000 (the default CMRI_PORT in XiaoNode-IP.ino).
   - Connection Prefix: C (default).
   - Connection Name: C/MRI (default).
   -  (If you need to add multiple network conected CMRI nodes, use the "+" next to the "CMRI" at the top of the page, and change the `CMRI_PORT 2000` port number in the sketch and in JMRI advanced properties section on the Add page)


2. Node Configuration
Click the Configure C/MRI Nodes button on the connection pane:

-  Node Address: 51 (matching NODE_ID in XiaoNode-IP.ino).
-  Node Type: Select CPNODE.
-  Receive Delay: 0 (default).
-  Assign IOX Ports Table:
   - 0x20: Port A = Input, Port B = Input (16 input bits)
   - 0x21: Port A = Output, Port B = Output (16 output bits)
   - 0x22: Port A = Output, Port B = Output (16 output bits)
   - 0x23: Port A = Output, Port B = Output (16 output bits)
-  Click Add Node (or Update Node), then Done.

3. Save & Restart
-  Click Save in Preferences.
-  **Restart** JMRI when prompted so the network client opens the socket connection.  None of this will work until you restart JMRI.

4. JMRI Hardware Addressing Reference
Once connected, JMRI addresses inputs and outputs by way of **turnout** and **sensor** tables.  You can use either `Node:Bit` format or classic system names (`CT51xxx` for turnouts, `CS51xxx` for sensors):

Outputs / Turnouts (64 bits total)
-  51:1 or 51:2 (CT51002): Onboard LED (phantom byte 0, bit 1)
-  51:17 ... 51:32 (CT51017 ... CT51032): Expander 0x21 (Ports A & B)
-  51:33 ... 51:48 (CT51033 ... CT51048): Expander 0x22 (Ports A & B)
-  51:49 ... 51:64 (CT51049 ... CT51064): Expander 0x23 (Ports A & B)

Inputs / Sensors (32 bits total)
-  51:3 (CS51003): Onboard Button on D2 (phantom byte 0, bit 2)
-  51:17 ... 51:32 (CS51017 ... CS51032): Expander 0x20 (Port A: 51:17–24, Port B: 51:25–32)

(Note: In JMRI, at least one sensor must be defined in the Sensor Table for JMRI's poll engine to begin polling the node for inputs.)