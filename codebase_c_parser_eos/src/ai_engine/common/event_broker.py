"""
ZeroMQ Event Broker (Dummy Wrapper)
===================================
A lightweight Pub/Sub message bus allowing decoupled data flow
between the AI Engine and the Control Plane Event Rules. 
"""

import zmq
import json
import threading
import logging

# Define explicit ZMQ Topics
TOPIC_AI_COORDS = "events/ai_coordinates"
TOPIC_ALERTS = "events/alerts"

log = logging.getLogger("event_broker")

class MQPublisher:
    def __init__(self, port: int = 5555):
        self.context = zmq.Context()
        self.socket = self.context.socket(zmq.PUB)
        self.socket.bind(f"tcp://127.0.0.1:{port}")
        log.debug(f"ZeroMQ Publisher bound to port {port}")

    def publish(self, topic: str, payload: dict):
        """Send asynchronous JSON events without caring who listens."""
        log.debug("MQ [PUB]: Processing topic '%s' with payload: %s", topic, payload)
        message = json.dumps(payload).encode('utf-8')
        # Format: "topic {json_data}"
        self.socket.send_multipart([topic.encode('utf-8'), message])
        log.debug("MQ [PUB]: Message sent to topic '%s'", topic)


class MQSubscriber:
    def __init__(self, port: int = 5555):
        self.context = zmq.Context()
        self.socket = self.context.socket(zmq.SUB)
        self.socket.connect(f"tcp://127.0.0.1:{port}")
        # Set a received timeout so the loop can check self._running
        self.socket.setsockopt(zmq.RCVTIMEO, 1000) # 1s timeout
        self.callbacks = []
        self._running = False

    def subscribe(self, topic: str):
        """Filter incoming messages by topic prefix."""
        self.socket.setsockopt_string(zmq.SUBSCRIBE, topic)
        log.debug(f"ZeroMQ Subscribed to: {topic}")

    def register_callback(self, callback):
        self.callbacks.append(callback)

    def stop(self):
        """Signal the listener loop to stop."""
        self._running = False
        # To break the recv_multipart blocking, we might need to send a dummy message or use poller
        # For simplicity in this dummy wrapper, we rely on the process exiting or 
        # a timeout if we implemented one. Let's send a dummy message if possible.
        # log.info("ZeroMQ Subscriber stopping...")
        log.debug("ZeroMQ Subscriber stopping...")

    def listen_forever(self):
        """Background loop to receive Pub/Sub and trigger callbacks."""
        self._running = True
        log.debug("ZeroMQ Listener started...")
        while self._running:
            try:
                # Blocks until message arrives or timeout (1s)
                topic_bytes, message_bytes = self.socket.recv_multipart()
                topic = topic_bytes.decode('utf-8')
                payload = json.loads(message_bytes.decode('utf-8'))
                
                log.debug("MQ [SUB]: Received message on topic '%s': %s", topic, payload)
                for cb in self.callbacks:
                    log.debug("MQ [SUB]: Triggering callback %s", cb.__name__ if hasattr(cb, '__name__') else cb)
                    cb(topic, payload)
            except zmq.Again:
                continue # Normal timeout, loop back and check self._running
            except Exception as e:
                log.error(f"MQ Listener Error: {e}")

    def start_background(self):
        t = threading.Thread(target=self.listen_forever, daemon=True)
        t.start()
