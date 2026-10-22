#!/usr/bin/env python3

import socket
import threading
import select

MODEM_HOST = "127.0.0.1"
MODEM_PORT = 6699

BUFFER_SIZE = 4096


class RetroWifiModem:
    def __init__(self):
        self.remote_socket = None

    def send_response(self, sock, text):
        sock.sendall((text + "\r\n").encode())

    def proxy_loop(self, modem_sock, remote_sock):
        print("Entering proxy mode")

        sockets = [modem_sock, remote_sock]

        try:
            while True:
                readable, _, _ = select.select(sockets, [], [])

                for s in readable:
                    data = s.recv(BUFFER_SIZE)

                    if not data:
                        raise ConnectionError()

                    if s is modem_sock:
                        remote_sock.sendall(data)
                    else:
                        modem_sock.sendall(data)

        except Exception:
            print("Connection closed")

        finally:
            try:
                remote_sock.close()
            except:
                pass

            self.remote_socket = None
            self.send_response(modem_sock, "NO CARRIER")

    def handle_dial(self, modem_sock, command):
        target = command[4:].strip()

        if ":" not in target:
            self.send_response(modem_sock, "ERROR")
            return

        host, port = target.rsplit(":", 1)

        try:
            port = int(port)
        except ValueError:
            self.send_response(modem_sock, "ERROR")
            return

        print(f"Dialing {host}:{port}")

        try:
            remote = socket.create_connection((host, port), timeout=10)
            self.remote_socket = remote

            self.send_response(modem_sock, "CONNECT")

            self.proxy_loop(modem_sock, remote)

        except Exception as e:
            print(f"Connect failed: {e}")
            self.send_response(modem_sock, "NO CARRIER")

    def process_command(self, modem_sock, command):
        command = command.strip().upper()

        print(f"AT Command: {command}")

        if command == "AT":
            self.send_response(modem_sock, "OK")

        elif command == "ATZ":
            self.send_response(modem_sock, "OK")

        elif command == "ATH":
            if self.remote_socket:
                self.remote_socket.close()
                self.remote_socket = None

            self.send_response(modem_sock, "OK")

        elif command.startswith("ATDT"):
            self.handle_dial(modem_sock, command)

        else:
            self.send_response(modem_sock, "ERROR")

    def run(self):
        while True:
            try:
                print(f"Connecting to modem at {MODEM_HOST}:{MODEM_PORT}")

                modem_sock = socket.create_connection(
                    (MODEM_HOST, MODEM_PORT)
                )

                print("Connected")

                buffer = b""

                while True:
                    data = modem_sock.recv(1024)

                    if not data:
                        break

                    buffer += data

                    while b"\r" in buffer or b"\n" in buffer:
                        if b"\r" in buffer:
                            line, buffer = buffer.split(b"\r", 1)
                        else:
                            line, buffer = buffer.split(b"\n", 1)

                        command = line.decode(
                            "ascii",
                            errors="ignore"
                        ).strip()

                        if command:
                            self.process_command(
                                modem_sock,
                                command
                            )

            except Exception as e:
                print(f"Disconnected: {e}")

            finally:
                try:
                    modem_sock.close()
                except:
                    pass


if __name__ == "__main__":
    RetroWifiModem().run()
