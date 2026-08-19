#!/usr/bin/env python3
"""
CMRInet Testbench Wiring & Hardware Architecture Diagram
Uses matplotlib & schemdraw for precise, crisp, professional technical rendering.
"""
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
import matplotlib.patches as patches
import numpy as np
from datetime import datetime

now = datetime.now()

# Set up figure
fig, ax = plt.subplots(figsize=(24, 13.5), dpi=150)
ax.set_xlim(-1.5, 23.5)
ax.set_ylim(-0.8, 14.5)
ax.set_aspect('equal')
ax.axis('off')

# Color palette
BG_COLOR = '#f8f9fa'
fig.patch.set_facecolor(BG_COLOR)
ax.set_facecolor(BG_COLOR)

C_POLL = '#d90429'        # Bold Red for Poll Pair +
C_POLL_N = '#ef233c'      # Lighter Red for Poll Pair -
C_REPLY = '#0077b6'       # Bold Blue for Reply Pair +
C_REPLY_N = '#0096c7'     # Lighter Blue for Reply Pair -
C_USB = '#495057'         # Dark Charcoal for USB
C_I2C = "#257d08ff"       # Green for I2C
C_JUMPER = "#7209b7"      # Purple for Jumpers
C_MAC = "#457b9d"         # Forest Green for Mac
C_MAC_TEXT = "#d6e8ea"    # Yellow for Mac text
C_BOARD_BG = '#e9ecef'    # Soft cool grey for boards
C_BOARD_BORDER = '#343a40'
C_PORT_IN = '#caf0f8'     # Soft cyan-blue for IN
C_PORT_OUT = '#ffe5d9'    # Soft peach-orange for OUT

DIR_FILL = {'IN': '#caf0f8', 'OUT': '#ffe5d9', 'UNUSED': '#eceff1'}
DIR_TEXT = {'IN': '#0d47a1', 'OUT': '#d00000', 'UNUSED': '#777777'}

# -------------------------------------------------------------
# 1. TOP RS-485 BUS LINES
# -------------------------------------------------------------
Y_POLL_P = 13.4
Y_POLL_N = 13.0
Y_REPL_P = 12.0
Y_REPL_N = 11.6

BUS_X_START = 1.0
BUS_X_END = 21.0

# Background banner for RS485 bus
bus_bg = patches.FancyBboxPatch((BUS_X_START - 1.0, 11.2), (BUS_X_END - BUS_X_START + 1.5), 2.6,
                                boxstyle="round,pad=0.1,rounding_size=0.3",
                                facecolor='#ffffff', edgecolor='#ced4da', linewidth=1.5, zorder=1)
ax.add_patch(bus_bg)

# Poll Pair lines (Host T± -> Node R±)
ax.plot([BUS_X_START, BUS_X_END], [Y_POLL_P, Y_POLL_P], color=C_POLL, lw=3.0, zorder=2)
ax.plot([BUS_X_START, BUS_X_END], [Y_POLL_N, Y_POLL_N], color=C_POLL_N, lw=2.0, linestyle='-', zorder=2)

# Reply Pair lines (Node T± -> Host R±)
ax.plot([BUS_X_START, BUS_X_END], [Y_REPL_P, Y_REPL_P], color=C_REPLY, lw=3.0, zorder=2)
ax.plot([BUS_X_START, BUS_X_END], [Y_REPL_N, Y_REPL_N], color=C_REPLY_N, lw=2.0, linestyle='-', zorder=2)

# Bus Labels
ax.text(7.3, Y_POLL_P - 0.3, 'RS-485 POLL PAIR (Host T±  ⟶  Node R±)  •  Carries I, T, P frames (Host ⟶ Node)',
        fontsize=10, fontweight='bold', color=C_POLL, ha='center', va='bottom', zorder=3)
ax.text(7.3, Y_REPL_N + 0.28, 'RS-485 REPLY PAIR (Node T±  ⟶  Host R±)  •  Carries R frames (Node ⟶ Host)',
        fontsize=10, fontweight='bold', color=C_REPLY, ha='center', va='top', zorder=3)

# Wire indicator tags on bus
ax.text(BUS_X_START - 0.3, Y_POLL_P, 'T+ / R+', fontsize=8, fontweight='bold', color=C_POLL, ha='right', va='center')
ax.text(BUS_X_START - 0.3, Y_POLL_N, 'T- / R-', fontsize=8, fontweight='bold', color=C_POLL_N, ha='right', va='center')
ax.text(BUS_X_START - 0.3, Y_REPL_P, 'R+ / T+', fontsize=8, fontweight='bold', color=C_REPLY, ha='right', va='center')
ax.text(BUS_X_START - 0.3, Y_REPL_N, 'R- / T-', fontsize=8, fontweight='bold', color=C_REPLY_N, ha='right', va='center')


# -------------------------------------------------------------
# 2. HELPER FUNCTIONS FOR DRAWING HARDWARE BOXES
# -------------------------------------------------------------
def draw_terminal(x, y, label, color):
    """Draw a screw terminal circle with label."""
    circle = plt.Circle((x, y), 0.12, facecolor=color, edgecolor='#212529', lw=1.2, zorder=6)
    ax.add_patch(circle)
    ax.text(x, y - 0.22, label, fontsize=8, fontweight='bold', color='#212529', ha='center', va='top', zorder=7)

def draw_xiao_board(x, y, w, h, title, role_text, usb_dev, is_host=False, is_node=False, is_sniffer=False):
    """Draw a cpNode-Xiao board with terminal block, MCU badge, and USB port."""
    # Main board body
    box = patches.FancyBboxPatch((x, y), w, h, boxstyle="round,pad=0.05,rounding_size=0.2",
                                facecolor=C_BOARD_BG, edgecolor=C_BOARD_BORDER, linewidth=2.0, zorder=4)
    ax.add_patch(box)

    # Title header banner
    header_color = '#1d3557' if is_host else ('#2b2d42' if is_node else '#457b9d')
    hdr = patches.FancyBboxPatch((x + 0.1, y + h - 1.0), w - 0.2, 0.6,
                                boxstyle="round,pad=0.02,rounding_size=0.1",
                                facecolor=header_color, edgecolor='none', zorder=5)
    ax.add_patch(hdr)
    ax.text(x + w/2, y + h - 0.7, title, fontsize=11, fontweight='bold', color='white', ha='center', va='center', zorder=6)

    # Subtitle / Role description
    ax.text(x + w/2, y + h - 1.1, role_text, fontsize=8, color='#333333', ha='center', va='top', zorder=5)

    # MCU & Hardware specs badge
    mcu_box = patches.Rectangle((x + 0.2, y + 0.6), w - 0.4, 0.85,
                                facecolor='#dee2e6', edgecolor='#adb5bd', linewidth=1, zorder=5)
    ax.add_patch(mcu_box)
    ax.text(x + w/2, y + 1.25, 'Seeed XIAO ESP32-C6', fontsize=8, fontweight='bold', color='#1d3557', ha='center', va='center', zorder=6)
    ax.text(x + w/2, y + 1.0, 'MAX3491 Half-Duplex RS-485', fontsize=7.5, color='#495057', ha='center', va='center', zorder=6)
    ax.text(x + w/2, y + 0.75, 'TX=D6, RX=D7, TXEN=D3\nSDA=D4 SCL=D5', fontsize=7, color='#6c757d', ha='center', va='center', zorder=6)

    # OLED badge if present
    oled_box = patches.Rectangle((x + w - 1.0, y + 0.2), 0.7, 0.3, facecolor='#212529', edgecolor='#495057', lw=0.8, zorder=5)
    ax.add_patch(oled_box)
    ax.text(x + w -0.7, y + 0.35, 'OLED\nDisplay', fontsize=6, color='white', ha='center', va='center', zorder=6)

    # USB-C port at bottom
    usb_x = x + w/2
    usb_box = patches.Rectangle((usb_x - 0.3, y - 0.12), 0.6, 0.12, facecolor='#495057', edgecolor='#212529', lw=1, zorder=5)
    ax.add_patch(usb_box)
    ax.text(usb_x, y - 0.06, 'USB-C', fontsize=6, color='white', ha='center', va='center', zorder=6)

    return {'x': x, 'y': y, 'w': w, 'h': h, 'usb_x': usb_x, 'usb_y': y - 0.12}


# -------------------------------------------------------------
# 3. DRAW DEVICE ROW (HOST, SNIFFER 1, SNIFFER 2, DONGLE, NODE)
# -------------------------------------------------------------
Y_DEV = 7.2
H_DEV = 3.6

# --- DEVICE 1: HOST (Left) ---
b_host = draw_xiao_board(1.0, Y_DEV, 3.4, H_DEV, 'HOST',
                         'cpNode-Xiao\nTX Drives Poll]nRX listens to Reply\nBaud: 28800 8N2',
                         'cu.usbmodem282201', is_host=True)

# Host Terminals on top edge (T-, T+, R-, R+, GND)
# Left-to-right order: T-, T+, R-, R+, GND
ht_tm = b_host['x'] + 0.5
ht_tp = b_host['x'] + 1.1
ht_rm = b_host['x'] + 1.8
ht_rp = b_host['x'] + 2.4
ht_gnd = b_host['x'] + 3.0
draw_terminal(ht_tm, Y_DEV + H_DEV, 'T-', C_POLL_N)
draw_terminal(ht_tp, Y_DEV + H_DEV, 'T+', C_POLL)
draw_terminal(ht_rm, Y_DEV + H_DEV, 'R-', C_REPLY_N)
draw_terminal(ht_rp, Y_DEV + H_DEV, 'R+', C_REPLY)
draw_terminal(ht_gnd, Y_DEV + H_DEV, 'SHIELD', '#6c757d')

# Host wires to bus (Drives Poll Pair, Receives Reply Pair)
ax.plot([ht_tp, ht_tp], [Y_DEV + H_DEV, Y_POLL_P], color=C_POLL, lw=2.5, zorder=3)
ax.plot([ht_tm, ht_tm], [Y_DEV + H_DEV, Y_POLL_N], color=C_POLL_N, lw=2.0, zorder=3)
ax.plot([ht_rp, ht_rp], [Y_DEV + H_DEV, Y_REPL_P], color=C_REPLY, lw=2.5, zorder=3)
ax.plot([ht_rm, ht_rm], [Y_DEV + H_DEV, Y_REPL_N], color=C_REPLY_N, lw=2.0, zorder=3)


# --- DEVICE 2: SNIFFER #1 (Poll Pair Tap) ---
b_sn1 = draw_xiao_board(5.2, Y_DEV, 3.3, H_DEV, 'SNIFFER #1\nPoll Pair witness',
                        'cpNode-Xiao\nPassive Tap on Host TX\nTX -nc-\nRX sees I, T, P frames',
                        'cu.usbmodem28101', is_sniffer=True)

sn1_tm = b_sn1['x'] + 0.5
sn1_tp = b_sn1['x'] + 1.1
sn1_rm = b_sn1['x'] + 1.8
sn1_rp = b_sn1['x'] + 2.4
sn1_gnd = b_sn1['x'] + 3.0
draw_terminal(sn1_tm, Y_DEV + H_DEV, 'T-', C_POLL_N)
draw_terminal(sn1_tp, Y_DEV + H_DEV, 'T+', C_POLL)
draw_terminal(sn1_rm, Y_DEV + H_DEV, 'R-', C_REPLY_N)
draw_terminal(sn1_rp, Y_DEV + H_DEV, 'R+', C_REPLY)
draw_terminal(sn1_gnd, Y_DEV + H_DEV, 'SHIELD', '#6c757d')

# Sniffer 1 tap lines up to Poll Pair (Dashed)
ax.plot([sn1_rp, sn1_rp], [Y_DEV + H_DEV, Y_POLL_P], color=C_POLL, lw=2.0, linestyle='--', zorder=3)
ax.plot([sn1_rm, sn1_rm], [Y_DEV + H_DEV, Y_POLL_N], color=C_POLL_N, lw=1.8, linestyle='--', zorder=3)
# Tap dots on bus
ax.plot(sn1_rp, Y_POLL_P, 'o', color=C_POLL, markersize=6, zorder=4)
ax.plot(sn1_rm, Y_POLL_N, 'o', color=C_POLL_N, markersize=6, zorder=4)


# --- DEVICE 3: SNIFFER #2 (Reply Pair Tap) ---
b_sn2 = draw_xiao_board(9.2, Y_DEV, 3.3, H_DEV, 'SNIFFER #2\nReply Pair witness',
                        'cpNode-Xiao\nPassive Tap on Node TX (Host R±)\nTX -nc-\nRX sees R frames\n120Ω Terminator on Reply Pair',
                        'cu.usbmodem2821301', is_sniffer=True)

sn2_tm = b_sn2['x'] + 0.5
sn2_tp = b_sn2['x'] + 1.1
sn2_rm = b_sn2['x'] + 1.8
sn2_rp = b_sn2['x'] + 2.4
sn2_gnd = b_sn2['x'] + 3.0
draw_terminal(sn2_tm, Y_DEV + H_DEV, 'T-', C_POLL_N)
draw_terminal(sn2_tp, Y_DEV + H_DEV, 'T+', C_POLL)
draw_terminal(sn2_rm, Y_DEV + H_DEV, 'R-', C_REPLY_N)
draw_terminal(sn2_rp, Y_DEV + H_DEV, 'R+', C_REPLY)
draw_terminal(sn2_gnd, Y_DEV + H_DEV, 'SHIELD', '#6c757d')


# Sniffer 2 tap lines up to Reply Pair (Dashed)
ax.plot([sn2_rp, sn2_rp], [Y_DEV + H_DEV, Y_REPL_P], color=C_REPLY, lw=2.0, linestyle='--', zorder=3)
ax.plot([sn2_rm, sn2_rm], [Y_DEV + H_DEV, Y_REPL_N], color=C_REPLY_N, lw=1.8, linestyle='--', zorder=3)
ax.plot(sn2_rp, Y_REPL_P, 'o', color=C_REPLY, markersize=6, zorder=4)
ax.plot(sn2_rm, Y_REPL_N, 'o', color=C_REPLY_N, markersize=6, zorder=4)


# --- DEVICE 4: RS-485 USB DONGLE (Reply Pair Witness) ---

d_x = 13.2
d_w = 2.6
d_box = patches.FancyBboxPatch((d_x, Y_DEV), d_w, H_DEV, boxstyle="round,pad=0.05,rounding_size=0.15",
                               facecolor='#f1faee', edgecolor='#1d3557', linewidth=2.0, zorder=4)
ax.add_patch(d_box)

d_hdr = patches.FancyBboxPatch((d_x + 0.1, Y_DEV + H_DEV - 1.0), d_w - 0.2, 0.6,
                               boxstyle="round,pad=0.02,rounding_size=0.1",
                               facecolor='#457b9d', edgecolor='none', zorder=5)
ax.add_patch(d_hdr)
ax.text(d_x + d_w/2, Y_DEV + H_DEV - 0.7, 'USB-RS485 DONGLE\nPoll Pair witness', fontsize=9.5, fontweight='bold', color='white', ha='center', va='center', zorder=6)

ax.text(d_x + d_w/2, Y_DEV + H_DEV - 1.1, 'Independent Hardware Tap\nFTDI / CH340 Bridge\nTX -nc-\nRX on REPLY pair\n28800 8N2 (DTR=RTS=1)', fontsize=7.5, color='#333', ha='center', va='top', zorder=5)

# Chip badge inside dongle
d_chip = patches.Rectangle((d_x + 0.3, Y_DEV + 0.65), d_w - 0.6, 0.7, facecolor='#dee2e6', edgecolor='#adb5bd', lw=1, zorder=5)
ax.add_patch(d_chip)
ax.text(d_x + d_w/2, Y_DEV + 1.0, 'USB-to-RS485\nTransceiver', fontsize=7, fontweight='bold', color='#1d3557', ha='center', va='center', zorder=6)

d_tm  = d_x + 0.5 - 0.2
d_tp  = d_x + 1.1 - 0.3
d_rm  = d_x + 1.8 - 0.5
d_rp  = d_x + 2.4 - 0.6
d_gnd = d_x + 3.0 - 0.7
draw_terminal(d_tm, Y_DEV + H_DEV, 'T-', C_POLL_N)
draw_terminal(d_tp, Y_DEV + H_DEV, 'T+', C_POLL)
draw_terminal(d_rm, Y_DEV + H_DEV, 'R-', C_REPLY_N)
draw_terminal(d_rp, Y_DEV + H_DEV, 'R+', C_REPLY)
draw_terminal(d_gnd, Y_DEV + H_DEV, 'SHIELD', '#6c757d')
# Dongle terminals A (+), B (-) on top edge
# d_b = d_x + 0.7
# d_a = d_x + 1.9
# draw_terminal(d_b, Y_DEV + H_DEV, 'B (-)', C_REPLY_N)
# draw_terminal(d_a, Y_DEV + H_DEV, 'A (+)', C_REPLY)

# Dongle taps up to Reply Pair (Dashed)
ax.plot([d_rm, d_rm], [Y_DEV + H_DEV, Y_REPL_P], color=C_REPLY, lw=2.0, linestyle='--', zorder=3)
ax.plot([d_rp, d_rp], [Y_DEV + H_DEV, Y_REPL_N], color=C_REPLY_N, lw=1.8, linestyle='--', zorder=3)
ax.plot(d_rm, Y_REPL_P, 'o', color=C_REPLY, markersize=6, zorder=4)
ax.plot(d_rp, Y_REPL_N, 'o', color=C_REPLY_N, markersize=6, zorder=4)

# USB port at bottom
d_usb_x = d_x + d_w/2
d_usb_box = patches.Rectangle((d_usb_x - 0.3, Y_DEV - 0.12), 0.6, 0.12, facecolor='#495057', edgecolor='#212529', lw=1, zorder=5)
ax.add_patch(d_usb_box)
ax.text(d_usb_x, Y_DEV - 0.06, 'USB-C', fontsize=6, color='white', ha='center', va='center', zorder=6)
b_dongle = {'usb_x': d_usb_x, 'usb_y': Y_DEV - 0.06}


# --- DEVICE 5: NODE (Right End) ---
b_node = draw_xiao_board(16.6, Y_DEV, 3.6, H_DEV, 'NODE',
                         'cpNode-Xiao\nUA 30  IOX32 + 3x cpNode-IOX\n7 Input Bytes / 7 Output Bytes',
                         'cu.usbmodem4101', is_node=True)

# Node Terminals on top edge (R+, R-, T+, T-, GND) — Crossover order!
# Crossover: Poll pair goes to Node R±; Node T± drives Reply pair!
nt_rp = b_node['x'] + 0.6
nt_rm = b_node['x'] + 1.2
nt_tp = b_node['x'] + 1.9
nt_tm = b_node['x'] + 2.5
nt_gnd = b_node['x'] + 3.1
draw_terminal(nt_rp, Y_DEV + H_DEV, 'R+', C_POLL)
draw_terminal(nt_rm, Y_DEV + H_DEV, 'R-', C_POLL_N)
draw_terminal(nt_tp, Y_DEV + H_DEV, 'T+', C_REPLY)
draw_terminal(nt_tm, Y_DEV + H_DEV, 'T-', C_REPLY_N)
draw_terminal(nt_gnd, Y_DEV + H_DEV, 'SHIELD', '#6c757d')

# Node wires up to bus (Straight horizontal crossover connections!)
ax.plot([nt_rp, nt_rp], [Y_DEV + H_DEV, Y_POLL_P], color=C_POLL, lw=2.5, zorder=3)
ax.plot([nt_rm, nt_rm], [Y_DEV + H_DEV, Y_POLL_N], color=C_POLL_N, lw=2.0, zorder=3)
ax.plot([nt_tp, nt_tp], [Y_DEV + H_DEV, Y_REPL_P], color=C_REPLY, lw=2.5, zorder=3)
ax.plot([nt_tm, nt_tm], [Y_DEV + H_DEV, Y_REPL_N], color=C_REPLY_N, lw=2.0, zorder=3)

# I2C Port at bottom-right of Node
node_i2c_x = 16.6 + 3.0
i2c_plug = patches.Rectangle((node_i2c_x - 0.25, Y_DEV - 0.12), 0.5, 0.12, facecolor=C_I2C, edgecolor='#212529', lw=1, zorder=5)
ax.add_patch(i2c_plug)
ax.text(node_i2c_x, Y_DEV - 0.06, 'I2C', fontsize=6, color='white', ha='center', va='center', zorder=6)


# -------------------------------------------------------------
# 4. MIDDLE TIER: USB BUS & MAC HOST
# -------------------------------------------------------------
Y_USB_BUS = 5.2

# Mac Host Box on Far Left
mac_x = -1.0
mac_w = 2.1
mac_h = 1.3
mac_box = patches.FancyBboxPatch((mac_x, Y_USB_BUS - 0.65), mac_w, mac_h, boxstyle="round,pad=0.05,rounding_size=0.15",
                                facecolor=C_MAC, edgecolor='#1b4332', linewidth=2.0, zorder=5)
ax.add_patch(mac_box)
ax.text(mac_x + mac_w/2, Y_USB_BUS + 0.25, 'MacOS', fontsize=9.5, fontweight='bold', color=C_MAC_TEXT, ha='center', va='center', zorder=6)
ax.text(mac_x + mac_w/2, Y_USB_BUS - 0.05, 'macOS USB Host', fontsize=8, color=C_MAC_TEXT, ha='center', va='center', zorder=6)
ax.text(mac_x + mac_w/2, Y_USB_BUS - 0.35, 'CDC Serial Console / CLI', fontsize=6.5, color=C_MAC_TEXT, ha='center', va='center', zorder=6)

# Main USB Bus Line running horizontally from Mac to Node
ax.plot([mac_x + mac_w, 19.5], [Y_USB_BUS, Y_USB_BUS], color=C_USB, lw=2.0, linestyle=':', zorder=2)
ax.text(4.0, Y_USB_BUS - 0.25, 'USB CDC Serial Bus (115200 baud stream & programming)', fontsize=8, color=C_USB, fontweight='bold', zorder=3)

# USB Drops from each device down to the USB bus line
devices_usb = [
    ('HOST', b_host['usb_x'], b_host['usb_y'], 'cu.usbmodem282201', 'Host CDC'),
    ('SNIFFER #1', b_sn1['usb_x'], b_sn1['usb_y'], 'cu.usbmodem28101', 'Sniffer 1 CDC'),
    ('SNIFFER #2', b_sn2['usb_x'], b_sn2['usb_y'], 'cu.usbmodem2821301', 'Sniffer 2 CDC'),
    ('DONGLE', b_dongle['usb_x'], b_dongle['usb_y'], 'cu.usbserial-BG04ID4L', 'RS-485 Dongle'),
    ('NODE', b_node['usb_x'], b_node['usb_y'], 'cu.usbmodem4101', 'Node CDC (drops off)'),
]

for name, ux, uy, dev_path, dev_note in devices_usb:
    # Dotted line from device USB port down to USB bus
    ax.plot([ux, ux], [uy, Y_USB_BUS], color=C_USB, lw=1.8, linestyle=':', zorder=2)
    ax.plot(ux, Y_USB_BUS, 's', color=C_USB, markersize=5, zorder=3)

    # Label badge near the connection
    badge = patches.FancyBboxPatch((ux - 1.0, Y_USB_BUS + 0.35), 2.0, 0.45,
                                  boxstyle="round,pad=0.02,rounding_size=0.08",
                                  facecolor='#ffffff', edgecolor='#adb5bd', lw=1, zorder=4)
    ax.add_patch(badge)
    ax.text(ux, Y_USB_BUS + 0.65, f'/dev/{dev_path}', fontsize=6.5, fontweight='bold', color='#212529', ha='center', va='center', zorder=5)
    ax.text(ux, Y_USB_BUS + 0.45, f'({dev_note})', fontsize=6, color='#6c757d', ha='center', va='center', zorder=5)


# -------------------------------------------------------------
# 5. LOWER TIER: I2C BUS & 5 MCP23017 EXPANDERS (ALL IN ONE ROW!)
# -------------------------------------------------------------
Y_I2C_BUS = 4.1
Y_EXP_TOP = 3.3
H_EXP = 2.8

# I2C Bus Line (Purple)
I2C_START_X = 5.6
I2C_END_X = 22.8

# Drop from Node I2C port down to I2C bus line
ax.plot([node_i2c_x, node_i2c_x], [Y_DEV - 0.12, Y_I2C_BUS], color=C_I2C, lw=2.2, zorder=2)
ax.plot(node_i2c_x, Y_I2C_BUS, 'o', color=C_I2C, markersize=6, zorder=3)

# Horizontal I2C Bus Line across all 5 expanders
ax.plot([I2C_START_X, I2C_END_X], [Y_I2C_BUS, Y_I2C_BUS], color=C_I2C, lw=2.2, zorder=2)
ax.text(I2C_START_X + 0.2, Y_I2C_BUS + 0.15, 'I2C Bus (SDA / SCL @ 3.3V) • Node ⟶ Expander Chain (Addresses 0x20 – 0x24)',
        fontsize=8.5, fontweight='bold', color=C_I2C, zorder=3)

# 5 Expanders Configuration Data:
# (address, board_type, portA_dir, portB_dir, notes, jumpers)
expander_data = [
    {
        'addr': '0x20',
        'board': 'IOX32 Card #1',
        'pA': 'IN', 'pA_bits': 'Bits 0..7 (Byte 2)',
        'pB': 'OUT', 'pB_bits': 'Bits 0..7 (Byte 2)',
        'notes': 'Unjumpered\nPure input reads',
        'jumpers': []
    },
    {
        'addr': '0x21',
        'board': 'IOX32 Card #2',
        'pA': 'IN', 'pA_bits': 'Bits 0..7 (Byte 3)',
        'pB': 'OUT', 'pB_bits': 'Bits 0..7 (Byte 3)',
        'notes': '2x Loopbacks:\nA1 ⟷ B1, A2 ⟷ B2',
        'jumpers': [(1, 1), (2, 2)]
    },
    {
        'addr': '0x22',
        'board': 'cpNode-IOX #1',
        'pA': 'IN', 'pA_bits': 'Bits 0..7 (Byte 4)',
        'pB': 'OUT', 'pB_bits': 'Bits 0..7 (Byte 4)',
        'notes': '8x Loopbacks:\nA[1-8] ⟷ B[1-8]',
        'jumpers': [(i, i) for i in range(1, 9)]
    },
    {
        'addr': '0x23',
        'board': 'cpNode-IOX #2',
        'pA': 'OUT', 'pA_bits': 'Bits 0..7 (Byte 5)',
        'pB': 'IN', 'pB_bits': 'Bits 0..7 (Byte 5)',
        'notes': 'Output A1 connected to 0x24:B1',
        'jumpers': [] # Handled separately
    },
    {
        'addr': '0x24',
        'board': 'cpNode-IOX #3',
        'pA': 'OUT', 'pA_bits': 'Bits 0..7 (Byte 6)',
        'pB': 'IN', 'pB_bits': 'Bits 0..7 (Byte 6)',
        'notes': 'Input B1 connected to 0x23:A1',
        'jumpers': []
    },
]

# Spacing for 5 expanders in one single horizontal row
EXP_W = 3.0
EXP_SPACING = 0.45
START_EXP_X = 6.0

exp_positions = {}

for idx, exp in enumerate(expander_data):
    ex = START_EXP_X + idx * (EXP_W + EXP_SPACING)
    ey = Y_EXP_TOP - H_EXP
    exp_positions[exp['addr']] = {'x': ex, 'y': ey, 'w': EXP_W, 'h': H_EXP}

    # Outer chip box
    c_box = patches.FancyBboxPatch((ex, ey), EXP_W, H_EXP, boxstyle="round,pad=0.03,rounding_size=0.15",
                                  facecolor='#ffffff', edgecolor='#343a40', linewidth=1.5, zorder=4)
    ax.add_patch(c_box)

    # I2C tap drop from I2C bus to chip top
    chip_i2c_x = ex + EXP_W / 2
    ax.plot([chip_i2c_x, chip_i2c_x], [ey + H_EXP, Y_I2C_BUS], color=C_I2C, lw=1.8, zorder=2)
    ax.plot(chip_i2c_x, ey + H_EXP, 'o', color=C_I2C, markersize=5, zorder=5)
    ax.plot(chip_i2c_x, Y_I2C_BUS, 'o', color=C_I2C, markersize=5, zorder=3)

    # Chip Header: MCP23017 Address
    chdr = patches.FancyBboxPatch((ex + 0.08, ey + H_EXP - 0.55), EXP_W - 0.16, 0.48,
                                 boxstyle="round,pad=0.02,rounding_size=0.08",
                                 facecolor='#495057', edgecolor='none', zorder=5)
    ax.add_patch(chdr)
    ax.text(ex + EXP_W/2, ey + H_EXP - 0.25, f"MCP23017 [{exp['addr']}]", fontsize=9, fontweight='bold', color='white', ha='center', va='center', zorder=6)
    ax.text(ex + EXP_W/2, ey + H_EXP - 0.43, exp['board'], fontsize=6.5, color='#adb5bd', ha='center', va='center', zorder=6)

    # Column A (Left) & Column B (Right) boxes
    col_w = (EXP_W - 0.24) / 2
    col_h = H_EXP - 0.65 - 0.65  # Leave space for notes at bottom

    # Port A box
    pa_bg = C_PORT_IN if exp['pA'] == 'IN' else C_PORT_OUT
    pa_box = patches.Rectangle((ex + 0.08, ey + 0.65), col_w, col_h, facecolor=pa_bg, edgecolor='#ced4da', lw=0.8, zorder=5)
    ax.add_patch(pa_box)
    ax.text(ex + 0.08 + col_w/2, ey + 0.65 + col_h - 0.2, f"Port A: {exp['pA']}", fontsize=7.5, fontweight='bold',
            color=DIR_TEXT[exp['pA']], ha='center', va='center', zorder=6)

    # Port B box
    pb_bg = C_PORT_IN if exp['pB'] == 'IN' else C_PORT_OUT
    pb_box = patches.Rectangle((ex + 0.16 + col_w, ey + 0.65), col_w, col_h, facecolor=pb_bg, edgecolor='#ced4da', lw=0.8, zorder=5)
    ax.add_patch(pb_box)
    ax.text(ex + 0.16 + col_w + col_w/2, ey + 0.65 + col_h - 0.2, f"Port B: {exp['pB']}", fontsize=7.5, fontweight='bold',
            color=DIR_TEXT[exp['pB']], ha='center', va='center', zorder=6)

    # 8 Pin indicators per port (A1..A8 on left, B1..B8 on right)
    pin_y_start = ey + 0.65 + col_h - 0.4
    pin_y_step = (col_h - 0.45) / 7

    for pin in range(1, 9):
        py = pin_y_start - (pin - 1) * pin_y_step
        # Pin dot A
        pax = ex + 0.2
        ax.plot(pax, py, 'o', color='#495057', markersize=3, zorder=7)
        ax.text(pax + 0.15, py, f'{pin}', fontsize=5.5, color='#495057', ha='left', va='center', zorder=7)

        # Pin dot B
        pbx = ex + EXP_W - 0.2
        ax.plot(pbx, py, 'o', color='#495057', markersize=3, zorder=7)
        ax.text(pbx - 0.15, py, f'{pin}', fontsize=5.5, color='#495057', ha='right', va='center', zorder=7)

    # Draw internal loopback jumpers (A pin to B pin)
    for p_a, p_b in exp['jumpers']:
        py_a = pin_y_start - (p_a - 1) * pin_y_step
        py_b = pin_y_start - (p_b - 1) * pin_y_step
        pax = ex + 0.2
        pbx = ex + EXP_W - 0.2
        # Clean horizontal jumper line with distinct jumper dots
        ax.plot([pax, pbx], [py_a, py_b], color=C_JUMPER, lw=1.8, zorder=8)
        ax.plot(pax, py_a, 'o', color=C_JUMPER, markersize=4.5, zorder=9)
        ax.plot(pbx, py_b, 'o', color=C_JUMPER, markersize=4.5, zorder=9)

    # Bottom notes banner inside chip
    ax.text(ex + EXP_W/2, ey + 0.32, exp['notes'], fontsize=6.5, color='#212529', ha='center', va='center', zorder=6)

# --- Cross-Expander Jumper: 0x23 Port A1 -> 0x24 Port B1 ---
e23_pos = exp_positions['0x23']
e24_pos = exp_positions['0x24']
col_w = (EXP_W - 0.24) / 2
col_h = H_EXP - 0.65 - 0.65
py_1 = e23_pos['y'] + 0.65 + col_h - 0.4

p_23_a1_x = e23_pos['x'] + 0.2
p_24_b1_x = e24_pos['x'] + EXP_W - 0.2

# Route jumper above the chips cleanly under the I2C bus
j_y_bridge = Y_I2C_BUS - 0.2
ax.plot([p_23_a1_x, p_23_a1_x, p_24_b1_x, p_24_b1_x],
        [py_1, j_y_bridge, j_y_bridge, py_1],
        color=C_JUMPER, lw=2.0, linestyle='-', zorder=8)
ax.plot(p_23_a1_x, py_1, 'o', color=C_JUMPER, markersize=5, zorder=9)
ax.plot(p_24_b1_x, py_1, 'o', color=C_JUMPER, markersize=5, zorder=9)
ax.text((p_23_a1_x + p_24_b1_x)/2, j_y_bridge - 0.2, 'Jumper: 0x23 Port A1 ⟶ 0x24 Port B1',
        fontsize=6.5, fontweight='bold', color=C_JUMPER, ha='center', va='bottom', zorder=9)


# -------------------------------------------------------------
# 6. LOWER LEFT: LEGEND & DIAGNOSTIC FINDINGS SUMMARY
# -------------------------------------------------------------
leg_x = -1.0
leg_w = 6.4
leg_h = 4.0 # 4.6
leg_y = 0.2

# Legend Container
leg_box = patches.FancyBboxPatch((leg_x, leg_y), leg_w, leg_h, boxstyle="round,pad=0.05,rounding_size=0.15",
                                facecolor='#ffffff', edgecolor='#adb5bd', linewidth=1.5, zorder=4)
ax.add_patch(leg_box)

# Header
ax.text(leg_x + leg_w/2, leg_y + leg_h - 0.35, 'SYSTEM ARCHITECTURE & WIRE LEGEND', fontsize=8.5, fontweight='bold', color='#1d3557', ha='center', va='center', zorder=5)

# Wire Legend Lines
wires = [
    ('RS-485 Poll Pair (I, T, P frames)', C_POLL, '-'),
    ('RS-485 Reply Pair (R reply frames)', C_REPLY, '-'),
    ('Passive Bus Tap', C_POLL, '--'),
    ('Passive Bus Tap', C_REPLY, '--'),
    ('USB CDC Bus & Telemetry Links', C_USB, ':'),
    ('I2C Bus (SDA / SCL @ 3.3V)', C_I2C, '-'),
    ('Hardware Jumper Wires', C_JUMPER, '-'),
]

for idx, (label, color, ls) in enumerate(wires):
    wy = leg_y + leg_h - 0.7 - idx * 0.28
    if ls == ':':
        ax.plot([leg_x + 0.3, leg_x + 1.2], [wy, wy], color=color, lw=2.0, linestyle=':', zorder=5)
    elif ls == '--':
        ax.plot([leg_x + 0.3, leg_x + 1.2], [wy, wy], color=color, lw=2.0, linestyle='--', zorder=5)
    else:
        ax.plot([leg_x + 0.3, leg_x + 1.2], [wy, wy], color=color, lw=2.2, linestyle='-', zorder=5)
    ax.text(leg_x + 1.4, wy, label, fontsize=7, color='#212529', va='center', zorder=5)

# Port Color Legend
py_leg = leg_y + 1.0
ax.text(leg_x + 0.3, py_leg, 'I/O Expander Port Direction:', fontsize=7.5, fontweight='bold', color='#212529', zorder=5)

p_in_patch = patches.Rectangle((leg_x + 0.3, py_leg - 0.35), 0.5, 0.22, facecolor=C_PORT_IN, edgecolor='#90e0ef', lw=1, zorder=5)
ax.add_patch(p_in_patch)
ax.text(leg_x + 0.9, py_leg - 0.24, 'IN (8-Bit Input, active-high reads as 1)', fontsize=6.5, color=DIR_TEXT['IN'], va='center', zorder=5)

p_out_patch = patches.Rectangle((leg_x + 3.2, py_leg - 0.35), 0.5, 0.22, facecolor=C_PORT_OUT, edgecolor='#f4a261', lw=1, zorder=5)
ax.add_patch(p_out_patch)
ax.text(leg_x + 3.8, py_leg - 0.24, 'OUT (8-Bit Output, 1 drives active-low)', fontsize=6.5, color=DIR_TEXT['OUT'], va='center', zorder=5)

# # Findings Box Divider
# ax.plot([leg_x + 0.3, leg_x + leg_w - 0.3], [leg_y + 1.35, leg_y + 1.35], color='#dee2e6', lw=1, zorder=5)
# ax.text(leg_x + 0.3, leg_y + 1.15, 'Key Diagnosis Finding (A/B Controlled Experiment):', fontsize=7.5, fontweight='bold', color='#d90429', zorder=5)
# ax.text(leg_x + 0.3, leg_y + 0.85, '• Under XiaoHostTracer: Reply pair carries R (809 frames on Host R± & Dongle)', fontsize=6.5, color='#212529', zorder=5)
# ax.text(leg_x + 0.3, leg_y + 0.55, '• Under SimpleHost: Reply pair silent (0 bytes on Host R± & Dongle; Host LED bitwalks)', fontsize=6.5, color='#212529', zorder=5)
# ax.text(leg_x + 0.3, leg_y + 0.25, '• Hardware & sniffer firmware verified intact; symptom is host-firmware dependent.', fontsize=6.5, color='#495057', zorder=5)
ax.text(leg_x + 0.3, leg_y + 0.25, f'• Generated {now.strftime("%I:%M %p %B %d, %Y")} by extras/bench/generate_diagram.py.', fontsize=6.5, color='#495057', zorder=5)

# Save Outputs
plt.tight_layout()
plt.savefig('./testbench_diagram.png', dpi=150, bbox_inches='tight', facecolor=fig.get_facecolor(), edgecolor='none')
plt.savefig('./testbench_diagram.svg', bbox_inches='tight', facecolor=fig.get_facecolor(), edgecolor='none')
print("Saved ./testbench_diagram.png and ./testbench_diagram.svg successfully!")
