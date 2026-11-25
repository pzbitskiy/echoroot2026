# Scenario: the normal life of the satellite. About two minutes.
mark "0 unmute, in case buttond remembers a mute"
key mute_off; sleep 1.5
mark "1 wake word + command, plain answer"
say time.raw; sleep 4
mark "2 wake word + command, streamed answer"
cmd streaming; sleep 0.3
say time2.raw; sleep 5
mark "3 announcement with chime, then one whose file is missing"
cmd announce; sleep 4
cmd announce_bad; sleep 2.5
mark "4 volume from HA, then from the volume ring"
cmd volume; sleep 1
ring vol_up; sleep 1
mark "5 'computer, stop' with nothing playing: request is dropped locally"
say stop.raw; sleep 2
mark "6 action button, nothing said"
cmd silent; sleep 0.3
key uber_press; sleep 0.2; key uber_release
quiet 3; sleep 1
mark "7 media from HA, the wake word interrupts it"
cmd media; sleep 2
say time.raw; sleep 4
mark "8 mute button on and off"
key mute_on; sleep 2; key mute_off; sleep 2
mark "9 wake word changed to Alexa by HA: 'Alexa' triggers, 'Computer' does not"
cmd ww_alexa; sleep 4
say alexa.raw; sleep 4
say time2.raw; sleep 2
cmd ww_computer; sleep 4
mark "10 question from HA (start_conversation), silence as the answer"
cmd silent; sleep 0.2; cmd ask
quiet 5; sleep 1
mark "11 timer finishes during a conversation, rings after it, 'computer, stop' silences it"
say time.raw & P=$!
sleep 4.2; cmd timer
wait $P; sleep 6
say stop.raw; sleep 2
mark "12 timer rings, action button silences it"
cmd timer; sleep 4
key uber_press; sleep 0.2; key uber_release; sleep 1
mark "13 HA drops the connection while we stream, then comes back"
say time.raw & P=$!
sleep 4.3; cmd drop
wait $P; sleep 4
say time2.raw; sleep 4
mark "14 mute from HA: no sound, wake word ignored; unmute from HA; mute by button, unmute from HA"
cmd mute_on; sleep 2
say time.raw; sleep 2
cmd mute_off; sleep 2
key mute_on; sleep 2
cmd mute_off; sleep 2
mark "15 timer rings, HA's Stop ringing button silences it; the button with nothing ringing"
cmd timer; sleep 4
cmd stop_ringing; sleep 1.5
cmd stop_ringing; sleep 1
