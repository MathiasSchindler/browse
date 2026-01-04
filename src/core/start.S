.global _start
.type _start, @function
_start:
	xor %rbp, %rbp
	andq $-16, %rsp
	call main
	mov %eax, %edi
	mov $60, %eax  # SYS_exit
	syscall
.size _start, . - _start
