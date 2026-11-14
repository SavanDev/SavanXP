/* L1 fixture: the first Linux executable. Libc-free, two system calls.
 * It prints "hello" and exits with status 7, so a launcher can tell its
 * status from a default 0 or a crash.
 *
 *   clang -nostdlib -static -fuse-ld=lld -o hello hello.S
 */
    .globl _start
    .text
_start:
    mov  $1, %eax            /* write */
    mov  $1, %edi
    lea  msg(%rip), %rsi
    mov  $6, %edx
    syscall
    mov  $60, %eax           /* exit */
    mov  $7, %edi
    syscall
msg: .ascii "hello\n"
