
initrd/service_manager.elf:     file format elf64-x86-64


Disassembly of section .text:

0000000000400000 <_start>:
  400000:	f3 0f 1e fa          	endbr64 
  400004:	41 b9 01 00 00 00    	mov    $0x1,%r9d
  40000a:	41 54                	push   %r12
  40000c:	be 40 01 40 00       	mov    $0x400140,%esi
  400011:	ba 26 00 00 00       	mov    $0x26,%edx
  400016:	55                   	push   %rbp
  400017:	4c 89 c8             	mov    %r9,%rax
  40001a:	4c 89 cf             	mov    %r9,%rdi
  40001d:	53                   	push   %rbx
  40001e:	0f 05                	syscall 
  400020:	bd d5 01 40 00       	mov    $0x4001d5,%ebp
  400025:	b8 29 00 00 00       	mov    $0x29,%eax
  40002a:	48 89 ef             	mov    %rbp,%rdi
  40002d:	0f 05                	syscall 
  40002f:	49 89 c0             	mov    %rax,%r8
  400032:	48 85 c0             	test   %rax,%rax
  400035:	0f 8e ec 00 00 00    	jle    400127 <_start+0x127>
  40003b:	be e4 01 40 00       	mov    $0x4001e4,%esi
  400040:	ba 19 00 00 00       	mov    $0x19,%edx
  400045:	4c 89 c8             	mov    %r9,%rax
  400048:	4c 89 cf             	mov    %r9,%rdi
  40004b:	0f 05                	syscall 
  40004d:	b8 29 00 00 00       	mov    $0x29,%eax
  400052:	bf fe 01 40 00       	mov    $0x4001fe,%edi
  400057:	0f 05                	syscall 
  400059:	48 89 c7             	mov    %rax,%rdi
  40005c:	48 85 c0             	test   %rax,%rax
  40005f:	7e 20                	jle    400081 <_start+0x81>
  400061:	b8 47 00 00 00       	mov    $0x47,%eax
  400066:	be 00 00 00 01       	mov    $0x1000000,%esi
  40006b:	0f 05                	syscall 
  40006d:	b8 01 00 00 00       	mov    $0x1,%eax
  400072:	be 90 01 40 00       	mov    $0x400190,%esi
  400077:	ba 1e 00 00 00       	mov    $0x1e,%edx
  40007c:	48 89 c7             	mov    %rax,%rdi
  40007f:	0f 05                	syscall 
  400081:	b8 29 00 00 00       	mov    $0x29,%eax
  400086:	bf 12 02 40 00       	mov    $0x400212,%edi
  40008b:	0f 05                	syscall 
  40008d:	48 89 c7             	mov    %rax,%rdi
  400090:	48 85 c0             	test   %rax,%rax
  400093:	7e 20                	jle    4000b5 <_start+0xb5>
  400095:	b8 47 00 00 00       	mov    $0x47,%eax
  40009a:	be 00 00 00 01       	mov    $0x1000000,%esi
  40009f:	0f 05                	syscall 
  4000a1:	b8 01 00 00 00       	mov    $0x1,%eax
  4000a6:	be 23 02 40 00       	mov    $0x400223,%esi
  4000ab:	ba 1b 00 00 00       	mov    $0x1b,%edx
  4000b0:	48 89 c7             	mov    %rax,%rdi
  4000b3:	0f 05                	syscall 
  4000b5:	b8 29 00 00 00       	mov    $0x29,%eax
  4000ba:	bf 3f 02 40 00       	mov    $0x40023f,%edi
  4000bf:	0f 05                	syscall 
  4000c1:	48 85 c0             	test   %rax,%rax
  4000c4:	7e 14                	jle    4000da <_start+0xda>
  4000c6:	b8 01 00 00 00       	mov    $0x1,%eax
  4000cb:	be 4c 02 40 00       	mov    $0x40024c,%esi
  4000d0:	ba 17 00 00 00       	mov    $0x17,%edx
  4000d5:	48 89 c7             	mov    %rax,%rdi
  4000d8:	0f 05                	syscall 
  4000da:	4c 8d 54 24 fc       	lea    -0x4(%rsp),%r10
  4000df:	be b0 01 40 00       	mov    $0x4001b0,%esi
  4000e4:	41 b9 2a 00 00 00    	mov    $0x2a,%r9d
  4000ea:	bb 01 00 00 00       	mov    $0x1,%ebx
  4000ef:	ba 24 00 00 00       	mov    $0x24,%edx
  4000f4:	41 bc 29 00 00 00    	mov    $0x29,%r12d
  4000fa:	66 0f 1f 44 00 00    	nopw   0x0(%rax,%rax,1)
  400100:	4c 89 c8             	mov    %r9,%rax
  400103:	4c 89 d7             	mov    %r10,%rdi
  400106:	0f 05                	syscall 
  400108:	49 39 c0             	cmp    %rax,%r8
  40010b:	75 f3                	jne    400100 <_start+0x100>
  40010d:	48 85 c0             	test   %rax,%rax
  400110:	7e ee                	jle    400100 <_start+0x100>
  400112:	48 89 d8             	mov    %rbx,%rax
  400115:	48 89 df             	mov    %rbx,%rdi
  400118:	0f 05                	syscall 
  40011a:	4c 89 e0             	mov    %r12,%rax
  40011d:	48 89 ef             	mov    %rbp,%rdi
  400120:	0f 05                	syscall 
  400122:	49 89 c0             	mov    %rax,%r8
  400125:	eb d9                	jmp    400100 <_start+0x100>
  400127:	be 68 01 40 00       	mov    $0x400168,%esi
  40012c:	ba 21 00 00 00       	mov    $0x21,%edx
  400131:	4c 89 c8             	mov    %r9,%rax
  400134:	4c 89 cf             	mov    %r9,%rdi
  400137:	0f 05                	syscall 
  400139:	e9 0f ff ff ff       	jmp    40004d <_start+0x4d>
