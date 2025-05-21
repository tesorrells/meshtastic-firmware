import meshtastic
from meshtastic import serial_interface
from meshtastic.protobuf import mesh_pb2, portnums_pb2, config_pb2
import time
from pubsub import pub
import argparse

iface = None
target_log_found = False
# Define the specific log message signature we are looking for
PHONEAPI_LOG_SIGNATURE = "PhoneAPI: Received SendTacticalMessageRequest"

# Callback function for pubsub messages
def general_on_receive(packet=None, interface=None, topic=pub.AUTO_TOPIC, line=None):
    global target_log_found
    topic_name = topic.getName()
    # print(f"DEBUG: PubSub event on topic: {topic_name}, packet: {packet}, line: {line}") # General debug

    if topic_name == "meshtastic.receive":
        # This topic usually provides fully decoded packets (often as dicts)
        if isinstance(packet, dict) and 'decoded' in packet and 'text' in packet['decoded']:
            log_line_content = packet['decoded']['text']
            print(f"DEVICE TEXT (via meshtastic.receive): {log_line_content}")
            if PHONEAPI_LOG_SIGNATURE in log_line_content:
                target_log_found = True
        # else:
            # print(f"DEBUG Unhandled meshtastic.receive packet structure: {packet}")

    elif topic_name == "meshtastic.log.line":
        # This topic is supposed to provide raw log lines
        if line is not None: # Check if the 'line' keyword argument was passed
            log_line_content = str(line) # Ensure it's a string
            print(f"DEVICE LOG (via meshtastic.log.line): {log_line_content.strip()}")
            if PHONEAPI_LOG_SIGNATURE in log_line_content:
                target_log_found = True
        # else:
            # print(f"DEBUG meshtastic.log.line called but 'line' was None. Packet: {packet}")

try:
    # Setup argument parser
    parser = argparse.ArgumentParser(description="Send a tactical message to a Meshtastic device.")
    parser.add_argument("--port", type=str, help="Serial port address of the Meshtastic device (e.g., /dev/ttyACM0)")
    parser.add_argument("-c", "--contact", type=int, default=0, help="Contact index (0-8, default: 0)")
    parser.add_argument("-d", "--distance", type=int, default=0, help="Distance index (0-8, default: 0)")
    parser.add_argument("-o", "--order", type=int, default=0, help="Order index (0-8, default: 0)")
    args = parser.parse_args()

    print(f"Using Contact Index: {args.contact}, Distance Index: {args.distance}, Order Index: {args.order}")
    print("Attempting to connect to device via USB...")

    # Subscribe to pubsub topics before creating the interface
    # This ensures we don't miss any initial messages or connection events.
    pub.subscribe(general_on_receive, "meshtastic.receive")
    pub.subscribe(general_on_receive, "meshtastic.log.line")
    print("Subscribed to pubsub topics: meshtastic.receive, meshtastic.log.line")

    iface = serial_interface.SerialInterface(devPath=args.port)
    print(f"Successfully connected to device via USB on port {args.port if args.port else 'auto-detected'}.")
    # Small delay to allow interface to settle and potentially receive initial connection data
    time.sleep(1) 

    to_radio_packet = mesh_pb2.ToRadio()
    to_radio_packet.send_tactical_message_request.contact_index = args.contact
    to_radio_packet.send_tactical_message_request.distance_index = args.distance
    to_radio_packet.send_tactical_message_request.order_index = args.order

    # constructed_packet_str = str(to_radio_packet).replace('\n', ' ') # Original problematic line's intent
    # print(f"Constructed ToRadio packet: {{ {constructed_packet_str} }}") # Original problematic line
    print(f"Constructed ToRadio packet (condensed): { {key: getattr(to_radio_packet, key) for key in to_radio_packet.DESCRIPTOR.fields_by_name if getattr(to_radio_packet, key)} }") # More robust compact print

    print("Sending SendTacticalMessageRequest via ToRadio packet...")

    # OLD METHOD - This wraps our ToRadio packet inside another MeshPacket's payload
    # iface.sendData(
    #     to_radio_packet,
    #     portNum=portnums_pb2.PortNum.Value('SERIAL_APP'),
    #     wantAck=True, # wantAck for sendData translates to MeshPacket.want_ack
    #     destinationId=iface.myInfo.my_node_num
    # )

    # NEW METHOD - This sends the ToRadio packet directly
    # The _sendToRadio method handles the ToRadio packet as the top-level message to the firmware.
    # It does not take portNum or destinationId because those are for MeshPackets.
    # ToRadio messages are implicitly for the local device's radio stack.
    # It also doesn't directly handle ACKs in the same way sendData does for MeshPackets.
    # For control messages like this, we usually just send and assume the device processes it.
    # If an ACK-like mechanism is needed, it would be part of the specific ToRadio/FromRadio handshake,
    # e.g. a specific response message in FromRadio.
    iface._sendToRadio(to_radio_packet)

    print("Request sent. Listening for device logs/responses for 10 seconds...")

    end_time = time.time() + 10
    while time.time() < end_time and not target_log_found:
        time.sleep(0.1) # Keep alive to allow pubsub to process events in its own thread

    if target_log_found:
        print(f"TARGET LOG SIGNATURE '{PHONEAPI_LOG_SIGNATURE}' FOUND.")
    else:
        print(f"TARGET LOG SIGNATURE '{PHONEAPI_LOG_SIGNATURE}' NOT found in received data within 10s.")

except Exception as e:
    print(f"An error occurred: {e}")
    import traceback
    traceback.print_exc()

finally:
    if iface:
        print("Closing interface.")
        # Unsubscribe from pubsub topics
        # Note: pub.unsubscribe can take a list of callables or a single one.
        # If general_on_receive was the only handler for these topics from this script, this is fine.
        try:
            pub.unsubscribe(general_on_receive, "meshtastic.receive")
            pub.unsubscribe(general_on_receive, "meshtastic.log.line")
            print("Unsubscribed from pubsub topics.")
        except Exception as e_unsub:
            print(f"Error during pubsub unsubscribe: {e_unsub}")
        
        iface.close() 