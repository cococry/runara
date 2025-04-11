CC=gcc
INCS=`pkg-config --cflags glfw3 cglm freetype2` -Ivendor/glad/include -Ivendor/stb_image/ 
LIBS=-lm -lGL -lfreetype -lharfbuzz 
CFLAGS+=${INCS} -O3 -ffast-math -Wno-stringop-overflow

# Compilation target (does NOT install anything)
all: lib/librunara.a

# Static library creation
lib/librunara.a: lib/runara.o lib/glad.o
	ar cr lib/librunara.a lib/runara.o lib/glad.o

# Object file compilation
lib/runara.o: runara.c | lib
	${CC} -c runara.c -o lib/runara.o ${CFLAGS}

lib/glad.o: vendor/glad/src/glad.c | lib
	${CC} -c vendor/glad/src/glad.c -o lib/glad.o ${CFLAGS}

# Ensure lib directory exists
lib:
	mkdir -p lib

# Clean target (remove built files)
clean:
	rm -rf lib

# Install compiled files (only executed with `sudo make install`)
install: all
	install -Dm644 lib/librunara.a /usr/local/lib/librunara.a
	install -d /usr/local/include/runara
	cp -r include/runara/* /usr/local/include/runara/
# Uninstall installed files
uninstall:
	rm -f /usr/local/lib/librunara.a
	rm -rf /usr/local/include/runara

.PHONY: all clean install uninstall
