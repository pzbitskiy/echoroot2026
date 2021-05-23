all: porcupine_demo_shm porcupine_send_shm
.PHONY: clean

clean:
	rm -f porcupine_demo_shm porcupine_send_shm

porcupine_demo_shm: porcupine_demo_shm.c
	../rootfs/br2echo/host/usr/bin/arm-buildroot-linux-gnueabi-gcc -std=c99 -O3 -Wall -o porcupine_demo_shm porcupine_demo_shm.c -I porcupine/include/ -ldl -lpthread

porcupine_send_shm: porcupine_send_shm.c
	../rootfs/br2echo/host/usr/bin/arm-buildroot-linux-gnueabi-gcc -std=c99 -O3 -Wall -o porcupine_send_shm porcupine_send_shm.c -I porcupine/include/ -ldl -lpthread
