import struct
import socket
import threading
import time
import requests
import queue
import cv2
import numpy as np
import logging

try:
    import av
    HAS_PYAV = True
except ImportError:
    HAS_PYAV = False

# Logger setup
logging.basicConfig(level=logging.INFO)
logger = logging.getLogger("SilkCastClient")

# Header structure: [frame_id:4][frag_id:2][num_frags:2][data_size:4]
HEADER_FORMAT = "<IHHI"
HEADER_SIZE = struct.calcsize(HEADER_FORMAT)

class SilkCastReceiver:
    def __init__(self, host, port=8080, device_id="video0", codec="mjpeg"):
        self.host = host
        self.api_port = port
        self.device_id = device_id
        self.codec = codec
        self.running = False
        self.udp_sock = None
        self.recv_thread = None
        
        self.latest_frame = None
        self.lock = threading.Lock()
        
        # Reassembly state
        self.current_frame_id = -1
        self.fragments = {} # frag_id -> data
        self.expected_frags = 0
        
        # Stats / Feedback
        self.last_frame_seq = -1
        self.last_idr_req_time = 0
        self.frames_received = 0
        
        # H264 Decoder
        self.av_codec_ctx = None
        if self.codec == "h264" and HAS_PYAV:
            self.av_codec = av.CodecContext.create('h264', 'r')

    def connect(self, udp_port=5000):
        """Starts the stream on the server and connects via UDP."""
        # 1. Setup local UDP socket
        self.udp_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        # Bind to any port to let OS choose, or specific if needed.
        # Actually we need to tell server where to send.
        # So we bind first.
        self.udp_sock.bind(("0.0.0.0", udp_port))
        local_ip = self._get_local_ip()
        _, actual_port = self.udp_sock.getsockname()
        
        # 2. Call API to start stream
        url = f"http://{self.host}:{self.api_port}/stream/udp/{self.device_id}"
        params = {
            "target": local_ip,
            "port": actual_port,
            "codec": self.codec,
            "w": 1280, "h": 720, "fps": 30, # Default params, can be parameterized
            "duration": 999999 # Long duration
        }
        logger.info(f"Requesting stream: {url} with {params}")
        try:
            res = requests.get(url, params=params, timeout=5)
            res.raise_for_status()
        except Exception as e:
            logger.error(f"Failed to start stream: {e}")
            raise

        logger.info(f"Stream started. Listening on UDP :{actual_port}")
        
        self.running = True
        self.recv_thread = threading.Thread(target=self._recv_loop, daemon=True)
        self.recv_thread.start()

    def read(self):
        """Returns the latest frame (numpy array) or None."""
        with self.lock:
            return self.latest_frame

    def request_idr(self):
        """Sends feedback to server requesting an IDR frame."""
        now = time.time()
        if now - self.last_idr_req_time < 1.0: # Rate limit 1s
            return
        logger.warning("Requesting IDR...")
        try:
            url = f"http://{self.host}:{self.api_port}/stream/{self.device_id}/feedback"
            requests.post(url, params={"type": "idr"}, timeout=1)
            self.last_idr_req_time = now
        except Exception as e:
            logger.error(f"Failed to send feedback: {e}")

    def _recv_loop(self):
        self.udp_sock.settimeout(1.0)
        while self.running:
            try:
                data, _ = self.udp_sock.recvfrom(65535)
                self._process_packet(data)
            except socket.timeout:
                continue
            except Exception as e:
                logger.error(f"Recv error: {e}")
                if not self.running: break
                
    def _process_packet(self, data):
        if len(data) < HEADER_SIZE:
            return

        # Parse Header
        frame_id, frag_id, num_frags, data_len = struct.unpack(HEADER_FORMAT, data[:HEADER_SIZE])
        payload = data[HEADER_SIZE:]
        
        if len(payload) != data_len:
            # logger.warning("Packet truncated")
            return

        # Check for new frame
        if frame_id != self.current_frame_id:
            # If we were building a frame and didn't finish, it's lost.
            if self.fragments:
                # Dropped frame logic
                logger.debug(f"Dropped frame {self.current_frame_id} (incomplete)")
                if self.codec == "h264":
                    # If we dropped a frame in H264, we likely need IDR if it was a reference frame.
                    # Simple heuristic: lost frames -> bad.
                    pass
            
            # Start new frame
            self.current_frame_id = frame_id
            self.fragments = {}
            self.expected_frags = num_frags
            
            # Gap check
            if self.last_frame_seq != -1 and frame_id > self.last_frame_seq + 1:
                logger.warning(f"Frame gap: {self.last_frame_seq} -> {frame_id}")
                if self.codec == "h264":
                    self.request_idr()
            self.last_frame_seq = frame_id

        # Store fragment
        self.fragments[frag_id] = payload
        
        # Check if complete
        if len(self.fragments) == self.expected_frags:
            # Reassemble
            full_data = bytearray()
            try:
                for i in range(self.expected_frags):
                    full_data.extend(self.fragments[i])
                
                self._decode_frame(full_data)
            except KeyError:
                logger.warning("Missing fragment during reassembly?")
            
            self.fragments = {} # Clear

    def _decode_frame(self, data):
        if self.codec == "mjpeg":
            # Decode MJPEG using OpenCV
            np_arr = np.frombuffer(data, np.uint8)
            img = cv2.imdecode(np_arr, cv2.IMREAD_COLOR)
            if img is not None:
                with self.lock:
                    self.latest_frame = img
        elif self.codec == "h264":
             if HAS_PYAV:
                 # PyAV decoding
                 packets = self.av_codec.parse(data)
                 for packet in packets:
                     frames = self.av_codec.decode(packet)
                     for frame in frames:
                         # Convert to numpy (OpenCV BGR)
                         img = frame.to_ndarray(format='bgr24')
                         with self.lock:
                             self.latest_frame = img
             else:
                 logger.warning("H264 received but PyAV not installed.")

    def _get_local_ip(self):
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        try:
            # doesn't even have to be reachable
            s.connect(('10.255.255.255', 1))
            IP = s.getsockname()[0]
        except Exception:
            IP = '127.0.0.1'
        finally:
            s.close()
        return IP

    def close(self):
        self.running = False
        if self.udp_sock:
            self.udp_sock.close()
        if self.recv_thread:
            self.recv_thread.join()

if __name__ == "__main__":
    # Demo usage
    client = SilkCastReceiver("127.0.0.1", codec="mjpeg")
    try:
        client.connect(udp_port=5000)
        print("Press Ctrl+C to stop...")
        while True:
            frame = client.read()
            if frame is not None:
                cv2.imshow("SilkCast Client", frame)
            
            if cv2.waitKey(1) & 0xFF == ord('q'):
                break
    except KeyboardInterrupt:
        pass
    finally:
        client.close()
        cv2.destroyAllWindows()
