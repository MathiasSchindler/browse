.global _start
.type _start, @function
_start:
	xor %rbp, %rbp
	/* Linux x86_64 process entry stack:
	 *   (%rsp)     = argc
	 *   8(%rsp)    = argv[0]
	 *   ...
	 */
	mov (%rsp), %rdi      # argc
	lea 8(%rsp), %rsi     # argv
	andq $-16, %rsp
	call main
	mov %eax, %edi
	mov $60, %eax  # SYS_exit
	syscall
.size _start, . - _start
