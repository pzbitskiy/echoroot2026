all: porcupine_demo_shm
.PHONY: clean

clean:
	rm -f porcupine_demo_shm

porcupine_demo_shm: porcupine_demo_shm.c
	arm-linux-gnueabi-gcc -std=c99 -O3 -Wall -o porcupine_demo_shm porcupine_demo_shm.c -I porcupine/include/ -ldl -lpthread
