CC = gcc
CFLAGS = -Wall -Wextra -pthread -g
INCL = include

all: initializer emitter receiver finalizer monitor

initializer: initializer.c
	$(CC) $(CFLAGS) -I$(INCL) initializer.c -o initializer

emitter: emitter.c
	$(CC) $(CFLAGS) -I$(INCL) emitter.c -o emitter

receiver: receiver.c
	$(CC) $(CFLAGS) -I$(INCL) receiver.c -o receiver

finalizer: finalizer.c
	$(CC) $(CFLAGS) -I$(INCL) finalizer.c -o finalizer
	
monitor: monitor.c
	$(CC) $(CFLAGS) -I$(INCL) monitor.c -o monitor

clean:
	rm -f initializer emitter receiver finalizer monitor
