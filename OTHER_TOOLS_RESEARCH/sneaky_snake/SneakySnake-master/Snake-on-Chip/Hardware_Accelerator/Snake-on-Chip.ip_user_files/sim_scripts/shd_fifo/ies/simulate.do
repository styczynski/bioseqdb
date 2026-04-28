set pack_assert_off {numeric_std std_logic_arith}

database -open waves -into waves.shm -default
probe -create -shm -all -variables -depth 1

run
exit
