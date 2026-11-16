# Commander Bluetooth core: host tests. make -f cmdr.mk test
CC ?= cc
CFLAGS = -std=gnu11 -Wall -Wextra -fsanitize=address,undefined -DAES256=1
CORE = core/cmdr.c core/cmdr_msg.c core/sec1.c core/pb.c third_party/monocypher.c third_party/aes.c

test:
	@mkdir -p build
	$(CC) $(CFLAGS) -o build/test_cmdr_msg test/test_cmdr_msg.c core/cmdr_msg.c core/pb.c && build/test_cmdr_msg
	$(CC) $(CFLAGS) -o build/test_sec1_session test/test_sec1_session.c core/sec1.c core/pb.c third_party/monocypher.c third_party/aes.c && build/test_sec1_session
	$(CC) $(CFLAGS) -o build/test_cmdr test/test_cmdr.c $(CORE) && build/test_cmdr

.PHONY: test
