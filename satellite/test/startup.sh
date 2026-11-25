# Scenario: start order, the sample configuration file, access control.
# Run with SAT_BEFORE_ASRD=1 NO_DYNCONF=1 and
#   SAT_OPTS="-c /media/t/out/echo-satellite.conf -o mdns_file=/tmp/echo.service -o allow=127.0.0.1
#             -o sound_wake=/usr/local/share/earcon/doppler/base/system_state_active_start.wav"
sleep 5
mark "1 asrd started after us and without dynconfig"
say time.raw; sleep 4
mark "2 a client that wants Noise encryption is turned away"
python -c "
import socket
s=socket.create_connection(('127.0.0.1',6053)); s.send('\x01\x00\x00'); print '[t] noise client got', repr(s.recv(10))"
cat /tmp/echo.service
