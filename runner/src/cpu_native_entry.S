/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Low-level transitions between the runner and the game (i386, AT&T syntax).
 *
 * fg_trap_entry: the SIGSEGV/SIGILL/SIGTRAP handler as registered with the
 *   kernel. The game may have any selector in %gs, but glibc keeps its TLS
 *   (and the stack-protector canary) at %gs, so restore the runner's %gs
 *   before entering C++. The kernel saved the game's %gs in the signal frame
 *   and restores it on sigreturn. %ds/%es/%ss are already flat here.
 *
 * fg_enter_game(const EntryRegs*): load the game's registers and transfer to
 *   CS:EIP with iret. The caller has already written the iret frame
 *   (EIP, CS, EFLAGS) at SS:ESP in game memory. Never returns.
 */
    .text

    .globl fg_trap_entry
    .type fg_trap_entry, @function
fg_trap_entry:
    movw fg_host_gs, %ax
    movw %ax, %gs
    jmp fg_trap_handler
    .size fg_trap_entry, . - fg_trap_entry

    .globl fg_enter_game
    .type fg_enter_game, @function
fg_enter_game:
    movl 4(%esp), %eax          /* EntryRegs* */
    movl 56(%eax), %ecx
    movw %cx, %gs
    movl 52(%eax), %ecx
    movw %cx, %fs
    movl 48(%eax), %ecx
    movw %cx, %es
    movl 60(%eax), %ecx
    movw %cx, %ss
    movl 28(%eax), %esp         /* game stack, iret frame on top */
    movl 4(%eax), %ebx
    movl 8(%eax), %ecx
    movl 12(%eax), %edx
    movl 16(%eax), %esi
    movl 20(%eax), %edi
    movl 24(%eax), %ebp
    pushl 0(%eax)               /* game eax (read through the flat %ds) */
    pushl 44(%eax)              /* game ds */
    popl %ds
    popl %eax
    iretl
    .size fg_enter_game, . - fg_enter_game

    .section .note.GNU-stack, "", @progbits
