import serial
import time
import threading
import sys
import _tracer_client

def read_sniffer(port_name, out_filename, stop_event):
    try:
        ser = serial.Serial(port_name, 115200, timeout=0.1)
        print(f"[{port_name}] Started logging to {out_filename}")
        with open(out_filename, "wb") as f:
            while not stop_event.is_set():
                if ser.in_waiting:
                    data = ser.read(ser.in_waiting)
                    f.write(data)
                    f.flush()
                else:
                    time.sleep(0.01)
        ser.close()
        print(f"[{port_name}] Finished.")
    except serial.SerialException as e:
        print(f"[{port_name}] ERROR: {e}")

def main():
    print("Starting sniffer captures...")
    print("Host RX: /dev/cu.usbmodem2821301")
    print("Host TX: /dev/cu.usbmodem28101")
    
    stop_event = threading.Event()
    t_rx = threading.Thread(target=read_sniffer, args=("/dev/cu.usbmodem2821301", "sniffer_rx.raw", stop_event))
    t_tx = threading.Thread(target=read_sniffer, args=("/dev/cu.usbmodem28101", "sniffer_tx.raw", stop_event))
    
    t_rx.start()
    t_tx.start()
    
    try:
        print("Running test script...")
        sys.argv = [sys.argv[0]]
        import gather_single_cycle
        result = gather_single_cycle.main()
        print(f"Test script finished with code {result}")
    except Exception as e:
        print(f"Test script error: {e}")
    finally:
        print("Stopping sniffers...")
        stop_event.set()
        t_rx.join()
        t_tx.join()
        print("Done.")

if __name__ == "__main__":
    main()
