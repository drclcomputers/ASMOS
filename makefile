all:
	nasm -f bin boot.asm -o boot.bin
	nasm -f bin kernel.asm -o kernel.bin

	cat boot.bin kernel.bin > os_image.bin

run: all
	qemu-system-i386 -drive format=raw,file=os_image.bin \
		-m 32M \
		-machine pc

clean:
	rm -f *.bin os_image.bin
