all:
	clang testlib.c -o testlib.so -shared -rdynamic -nostdlib -fPIC
	clang -g main.c testlib.so -o main -nostdlib -lc -e main
	clang++ -g -std=c++23 -stdlib=libc++ loader.cpp -o loader -I include/

clean:
	rm testlib.so main loader
