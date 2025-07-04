
extern int testexport(int res);
extern const char* nametest;

extern int puts(const char* str);
extern void exit(int code);

void main(){

	puts("HelloWorld!");
	puts(nametest);

	volatile int res = testexport(3);

	__asm__ volatile (
		"xor %%rdi, %%rdi\n"
		"mov %0, %%rax\n"
		"syscall"
		: : "i"(0x3c)
	);
	
}
