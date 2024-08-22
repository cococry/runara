CC=gcc
INCS=`pkg-config --cflags glfw3 cglm` -Ivendor/glad/include -Ivendor/stb_image/ -Ivendor/uthash/include
LIBS=-lclipboard -lm -lGL -lfreetype -lharfbuzz -lfontconfig
CFLAGS+=${INCS} ${LIBS} -O3 -ffast-math
all: lib/runara.a
lib/runara.a: lib/runara.o
	ar cr lib/librunara.a lib/*.o
lib/runara.o: lib
	${CC} -c runara.c -o lib/runara.o ${CFLAGS}
	${CC} -c vendor/glad/src/glad.c -o lib/glad.o  ${CFLAGS}
lib:
	mkdir lib
clean:
	rm -r ./lib

install: all
	cp lib/librunara.a /usr/local/lib/ 
	cp -r include/runara /usr/local/include/ 
	cp -r ./vendor/uthash/include/uthash /usr/local/include

uninstall:
	rm -f /usr/local/lib/librunara.a
	rm -rf /usr/local/include/runara/

.PHONY: all test clean
