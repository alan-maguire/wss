
PROGS = wss-v1 wss-v2 wss-v3 testmem

CFLAGS += -O1

all: $(PROGS)
	
clean:
	rm -f $(PROGS)
