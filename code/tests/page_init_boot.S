/* Test-only boot code: identity mappings allow the satp transition and UART I/O. */
#include <mmu.h>
#include <memlayout.h>

.section .text.kern_entry,"ax",%progbits
.globl kern_entry
kern_entry:
    la t0, boot_page_table_sv39
    srli t0, t0, PGSHIFT
    li t1, 8 << 60
    or t0, t0, t1
    csrw satp, t0
    sfence.vma
    lui sp, %hi(bootstacktop)
    addi sp, sp, %lo(bootstacktop)
    lui t0, %hi(kern_init)
    addi t0, t0, %lo(kern_init)
    jr t0

.section .data
.balign PGSIZE
.globl bootstack
bootstack:
    .space KSTACKSIZE
.globl bootstacktop
bootstacktop:

.balign PGSIZE
.globl boot_page_table_sv39
boot_page_table_sv39:
    .quad 0xcf                         /* VA 0 -> PA 0: test UART */
    .quad 0
    .quad (0x80000 << 10) | 0xcf       /* VA 0x80000000 -> same PA */
    .zero 8 * 508
    .quad (0x80000 << 10) | 0xcf       /* high kernel mapping */
