all: spork.o main.o
	g++ -m64 -lmozjs -lnspr4 \
		-L ../../drd-js/js/src/build-debug/dist/lib \
		-o spork main.o spork.o

%.o: %.cpp
	g++ -w -m64 \
		-I/usr/local/include/nspr \
		-I ../../drd-js/js/src/build-debug/dist/include \
		-o $@ -c $<
