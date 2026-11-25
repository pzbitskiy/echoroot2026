# Scenario: wake-word model selected by restarting asrd, the way it works on
# a real Echo (the WWModel property is refused there).
# Run with ASRD_LOOP=1 and
#   SAT_OPTS="-o wake_word=alexa -o asr_runtime_switch=0 -o asr_restart_cmd=/tmp/restart-asrd"
# asrd starts with COMPUTER (preselected by the harness); the daemon wants ALEXA.
sleep 16
mark "1 alexa after the restart"
say alexa.raw; sleep 5
mark "2 the old word does nothing"
say time.raw; sleep 4
mark "3 Home Assistant selects computer"
cmd ww_computer; sleep 16
say time.raw; sleep 5
get-dynconf-value asrd.model
