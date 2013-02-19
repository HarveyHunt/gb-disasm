#include "stdinc.h"
#include "rom.h"
#include "sops.h"
#include "state.h"

/*
	$FFFF 	        Interrupt Enable Flag
	$FF80-$FFFE     Zero Page - 127 bytes
	$FF00-$FF7F     Hardware I/O Registers
	$FEA0-$FEFF     Unusable Memory
	$FE00-$FE9F     OAM - Object Attribute Memory
	$E000-$FDFF     Echo RAM - Reserved, Do Not Use
	$D000-$DFFF     Internal RAM - Bank 1-7 (switchable - CGB only)
	$C000-$CFFF     Internal RAM - Bank 0 (fixed)
	$A000-$BFFF     Cartridge RAM (If Available)
	$9C00-$9FFF     BG Map Data 2
	$9800-$9BFF     BG Map Data 1
	$8000-$97FF     Character RAM
	$4000-$7FFF     Cartridge ROM - Switchable Banks 1-xx
	$0150-$3FFF     Cartridge ROM - Bank 0 (fixed)
	$0100-$014F     Cartridge Header Area
	$0000-$00FF     Restart and Interrupt Vectors
*/


/* GLOBALS - start */

/** ROM raw content. */
rom*		r;
/** MBC type. */
uint8_t		mbc;

/** Temporary buffer for formatting. */
char		tmp[128];

/** Current memory bank. */
int			bank;
/** Program counter. */
uint16_t	pc;
/** Register A. */
uint8_t		a;
/** 0xFF00-0xFFFF used for LDH operation. TODO: check what LDH really does */
uint8_t     hmem[0xFF];

/** Operations list. */
op*						sops;
/** When branching, state to set back when returning. */
state*                  top;
/** Adresses for jmps and calls, used for labelling. TODO: replace with ANSI C container */
std::vector<uint32_t>	jmp_addr;
std::vector<uint32_t>	call_addr;

/** Physical address mapping. */
uint32_t phy(uint16_t addr) {
	/* ROM only */
	if(mbc == 0x00 || mbc == 0x08 || mbc == 0x09)
		return addr;
	/* TODO: for the moment we don't check the mbc, and threat MBC1, MBC3 and MBC5 equally */
	if(addr < 0x4000) return addr;
	return ((bank-1) * 0x4000) + addr;
}

/** Conditional jump #1. */
void jmp(uint16_t addr) {
	if(addr < 0x8000 && phy(addr) < r->total) {
	    top = state_push(top, pc + 3, bank);
		pc = addr;
	} else {
		printf("[0x.8X] Warning: Address too high, ignoring 0x%.4X (0x%.8X)\n", 
		    pc, addr, phy(addr));
		pc += 3;
	}
}

/** Conditional jump #2. */
void jmp(uint8_t addr) {
	uint16_t new_pc = pc + ((char)addr) + 2;
	if(new_pc < 0x8000) {
        top = state_push(top, pc + 2, bank);
		pc = new_pc;
	} else {
		printf("[0x.8X] Warning: Address too high, ignoring 0x%.4X (0x%.8X)\n", 
		    pc, new_pc, phy(new_pc));
		pc += 2;
	}
}

/** Unconditional jump #1. */
void jmpu(uint16_t addr) {
	if(addr < 0x8000 && phy(addr) < r->total)
		pc = addr;
	else {
		printf("[0x.8X] Warning: Address too high, ignoring 0x%.4X (0x%.8X)\n", 
		    pc, addr, phy(addr));
		pc = 0x100;
	}
}

/** Unconditional jump #2. */
void jmpu(uint8_t addr) {
	uint16_t new_pc = pc + ((char)addr) + 2;
	if(new_pc < 0x8000 && phy(new_pc) < r->total)
		pc = new_pc;
	else {
		printf("[0x.8X] Warning: Address too high, ignoring 0x%.4X (0x%.8X)\n", 
		    pc, new_pc, phy(new_pc));
		pc = 0x100;
	}
}

/** Return from call or jump. */
void ret() {
    pc = top->pc;	    
	bank = top->bank;
	top = state_pop(top);
}

/** No-arg operator */
op* op_0(const char* opname) {
	return op_create(phy(pc), &(r->raw[phy(pc)]), 1, opname);
}

op* op_0_2(const char* opname) {
	return op_create(phy(pc), &(r->raw[phy(pc)]), 2, opname);
}

/* Adresses as right arg */
op* op_r(const char* opname, uint16_t addr) {
	sprintf(tmp, "%s,$%.4X", opname, addr);
	return op_create(phy(pc), &(r->raw[phy(pc)]), 3, tmp);
}

op* op_rb(const char* opname, uint16_t addr) {
	sprintf(tmp, "%s,($%.4X)", opname, addr);
	return op_create(phy(pc), &(r->raw[phy(pc)]), 3, tmp);
}

op* op_r(const char* opname, uint8_t addr) {
	sprintf(tmp, "%s,$%.2X", opname, addr);
	return op_create(phy(pc), &(r->raw[phy(pc)]), 2, tmp);
}

op* op_rb(const char* opname, uint8_t addr) {
	sprintf(tmp, "%s,($%.2X)", opname, addr);
	return op_create(phy(pc), &(r->raw[phy(pc)]), 2, tmp);
}

/* Adresses as left arg */
op* op_l(const char* opname, uint16_t addr, const char* right) {
	sprintf(tmp, "%s $%.4X%s", opname, addr, right);
	return op_create(phy(pc), &(r->raw[phy(pc)]), 3, tmp);
}

op* op_lb(const char* opname, uint16_t addr, const char* right) {
	sprintf(tmp, "%s ($%.4X)%s", opname, addr, right);
	return op_create(phy(pc), &(r->raw[phy(pc)]), 3, tmp);
}

op* op_l(const char* opname, uint8_t addr, const char* right) {
	sprintf(tmp, "%s $%.2X%s", opname, addr, right);
	return op_create(phy(pc), &(r->raw[phy(pc)]), 2, tmp);
}

op* op_lb(const char* opname, uint8_t addr, const char* right) {
	sprintf(tmp, "%s ($%.2X)%s", opname, addr, right);
	return op_create(phy(pc), &(r->raw[phy(pc)]), 2, tmp);
}

void print_dump(FILE* f) {
	for(unsigned int i=0; i<jmp_addr.size(); i++) {
		sops_set_flag(sops, jmp_addr[i], OP_FLAG_JMP_ADDR);
	}
	for(unsigned int i=0; i<call_addr.size(); i++) {
		sops_set_flag(sops, jmp_addr[i], OP_FLAG_JMP_ADDR);
	}
	sops_dump(sops, f);		
}

void print_dump(const char* file_name) {
	FILE* f = fopen(file_name, "w");
	if(f) {
		print_dump(f);
		fclose(f);
	}
}

void print_dump() {
	print_dump(stdout);
}

/* TODO: printing asm code is definetely not finished */
void print_asm(FILE* f) {
	for(unsigned int i=0; i<jmp_addr.size(); i++) {
		sops_set_flag(sops, jmp_addr[i], OP_FLAG_JMP_ADDR);
	}
	for(unsigned int i=0; i<call_addr.size(); i++) {
		sops_set_flag(sops, jmp_addr[i], OP_FLAG_JMP_ADDR);
	}
	sops_asm(sops, f);	
}

void print_asm(const char* file_name) {
	FILE* f = fopen(file_name, "w");
	if(f) {
		print_asm(f);
		fclose(f);
	}
}

void print_asm() {
	print_asm(stdout);
}

/** Remember the times when you put everything in main? They come back! */
int main(int argc, char** argv) {
	uint8_t		addr8;
	uint16_t	addr16;

	if(argc != 2) {
		printf("Usage: %s <ROM>\n", argv[0]);
		return -1;
	}
	
	r = rom_load(argv[1]);
	if(!r) {
		puts("Could not load file");
		return -2;
	}

	rom_info(r);
	
   	sops = NULL;
	top = NULL;

	/* Start address is always 0x100 */
	pc = 0x100;
	/* Bank 0 (default) == Bank 1 */
	bank = 1;

	while(1) {
	    /* Do not visit same instruction twice */
		if(sops_contains(sops, phy(pc))) {
		    /* Check if we have any other possible branches to follow */
	        if(top) {
				pc = top->pc;
				bank = top->bank;
                top = state_pop(top);
	        } else {
	            puts("Finished succesfully");
	            goto finish;
            }
        }            
           
		/* big switch interpreting the operations */
		switch(r->raw[phy(pc)]) {
            /* AUTOGENERATED - look at generator.py */
            /* NOP */
            case 0x0:
                sops = sops_add(sops, op_0("NOP"));
                pc += 1;
                break;
            /* LD BC,d16 */
            case 0x1:
                addr16 = r->raw[phy(pc+1)] | (r->raw[phy(pc+2)]<<8);
                sops = sops_add(sops, op_r("LD BC", addr16));
                pc += 3;
                break;
            /* LD (BC),A */
            case 0x2:
                sops = sops_add(sops, op_0("LD (BC),A"));
                pc += 1;
                break;
            /* INC BC */
            case 0x3:
                sops = sops_add(sops, op_0("INC BC"));
                pc += 1;
                break;
            /* INC B */
            case 0x4:
                sops = sops_add(sops, op_0("INC B"));
                pc += 1;
                break;
            /* DEC B */
            case 0x5:
                sops = sops_add(sops, op_0("DEC B"));
                pc += 1;
                break;
            /* LD B,d8 */
            case 0x6:
                addr8 = r->raw[phy(pc+1)];
                sops = sops_add(sops, op_r("LD B", addr8));
                pc += 2;
                break;
            /* RLCA */
            case 0x7:
                sops = sops_add(sops, op_0("RLCA"));
                pc += 1;
                break;
            /* LD (a16),SP */
            case 0x8:
                addr16 = r->raw[phy(pc+1)] | (r->raw[phy(pc+2)]<<8);
                sops = sops_add(sops, op_lb("LD", addr16, ",SP"));
                pc += 3;
                break;
            /* ADD HL,BC */
            case 0x9:
                sops = sops_add(sops, op_0("ADD HL,BC"));
                pc += 1;
                break;
            /* LD A,(BC) */
            case 0xa:
                sops = sops_add(sops, op_0("LD A,(BC)"));
                pc += 1;
                break;
            /* DEC BC */
            case 0xb:
                sops = sops_add(sops, op_0("DEC BC"));
                pc += 1;
                break;
            /* INC C */
            case 0xc:
                sops = sops_add(sops, op_0("INC C"));
                pc += 1;
                break;
            /* DEC C */
            case 0xd:
                sops = sops_add(sops, op_0("DEC C"));
                pc += 1;
                break;
            /* LD C,d8 */
            case 0xe:
                addr8 = r->raw[phy(pc+1)];
                sops = sops_add(sops, op_r("LD C", addr8));
                pc += 2;
                break;
            /* RRCA */
            case 0xf:
                sops = sops_add(sops, op_0("RRCA"));
                pc += 1;
                break;
            /* STOP 0 */
            case 0x10:
                addr8 = r->raw[phy(pc+1)];
                pc += 2;
                break;
            /* LD DE,d16 */
            case 0x11:
                addr16 = r->raw[phy(pc+1)] | (r->raw[phy(pc+2)]<<8);
                sops = sops_add(sops, op_r("LD DE", addr16));
                pc += 3;
                break;
            /* LD (DE),A */
            case 0x12:
                sops = sops_add(sops, op_0("LD (DE),A"));
                pc += 1;
                break;
            /* INC DE */
            case 0x13:
                sops = sops_add(sops, op_0("INC DE"));
                pc += 1;
                break;
            /* INC D */
            case 0x14:
                sops = sops_add(sops, op_0("INC D"));
                pc += 1;
                break;
            /* DEC D */
            case 0x15:
                sops = sops_add(sops, op_0("DEC D"));
                pc += 1;
                break;
            /* LD D,d8 */
            case 0x16:
                addr8 = r->raw[phy(pc+1)];
                sops = sops_add(sops, op_r("LD D", addr8));
                pc += 2;
                break;
            /* RLA */
            case 0x17:
                sops = sops_add(sops, op_0("RLA"));
                pc += 1;
                break;
            /* JR r8 */
            case 0x18:
                addr8 = r->raw[phy(pc+1)];
                sops = sops_add(sops, op_r("JR", addr8));
                jmp_addr.push_back(phy(pc + ((char)addr8)));
                jmpu(addr8);
                break;
            /* ADD HL,DE */
            case 0x19:
                sops = sops_add(sops, op_0("ADD HL,DE"));
                pc += 1;
                break;
            /* LD A,(DE) */
            case 0x1a:
                sops = sops_add(sops, op_0("LD A,(DE)"));
                pc += 1;
                break;
            /* DEC DE */
            case 0x1b:
                sops = sops_add(sops, op_0("DEC DE"));
                pc += 1;
                break;
            /* INC E */
            case 0x1c:
                sops = sops_add(sops, op_0("INC E"));
                pc += 1;
                break;
            /* DEC E */
            case 0x1d:
                sops = sops_add(sops, op_0("DEC E"));
                pc += 1;
                break;
            /* LD E,d8 */
            case 0x1e:
                addr8 = r->raw[phy(pc+1)];
                sops = sops_add(sops, op_r("LD E", addr8));
                pc += 2;
                break;
            /* RRA */
            case 0x1f:
                sops = sops_add(sops, op_0("RRA"));
                pc += 1;
                break;
            /* JR NZ,r8 */
            case 0x20:
                addr8 = r->raw[phy(pc+1)];
                sops = sops_add(sops, op_r("JR NZ", addr8));
                jmp_addr.push_back(phy(pc + ((char)addr8)));
                jmp(addr8);
                break;
            /* LD HL,d16 */
            case 0x21:
                addr16 = r->raw[phy(pc+1)] | (r->raw[phy(pc+2)]<<8);
                sops = sops_add(sops, op_r("LD HL", addr16));
                pc += 3;
                break;
            /* LD (HL+),A */
            case 0x22:
                sops = sops_add(sops, op_0("LD (HL+),A"));
                pc += 1;
                break;
            /* INC HL */
            case 0x23:
                sops = sops_add(sops, op_0("INC HL"));
                pc += 1;
                break;
            /* INC H */
            case 0x24:
                sops = sops_add(sops, op_0("INC H"));
                pc += 1;
                break;
            /* DEC H */
            case 0x25:
                sops = sops_add(sops, op_0("DEC H"));
                pc += 1;
                break;
            /* LD H,d8 */
            case 0x26:
                addr8 = r->raw[phy(pc+1)];
                sops = sops_add(sops, op_r("LD H", addr8));
                pc += 2;
                break;
            /* DAA */
            case 0x27:
                sops = sops_add(sops, op_0("DAA"));
                pc += 1;
                break;
            /* JR Z,r8 */
            case 0x28:
                addr8 = r->raw[phy(pc+1)];
                sops = sops_add(sops, op_r("JR Z", addr8));
                jmp_addr.push_back(phy(pc + ((char)addr8)));
                jmp(addr8);
                break;
            /* ADD HL,HL */
            case 0x29:
                sops = sops_add(sops, op_0("ADD HL,HL"));
                pc += 1;
                break;
            /* LD A,(HL+) */
            case 0x2a:
                sops = sops_add(sops, op_0("LD A,(HL+)"));
                pc += 1;
                break;
            /* DEC HL */
            case 0x2b:
                sops = sops_add(sops, op_0("DEC HL"));
                pc += 1;
                break;
            /* INC L */
            case 0x2c:
                sops = sops_add(sops, op_0("INC L"));
                pc += 1;
                break;
            /* DEC L */
            case 0x2d:
                sops = sops_add(sops, op_0("DEC L"));
                pc += 1;
                break;
            /* LD L,d8 */
            case 0x2e:
                addr8 = r->raw[phy(pc+1)];
                sops = sops_add(sops, op_r("LD L", addr8));
                pc += 2;
                break;
            /* CPL */
            case 0x2f:
                sops = sops_add(sops, op_0("CPL"));
                pc += 1;
                break;
            /* JR NC,r8 */
            case 0x30:
                addr8 = r->raw[phy(pc+1)];
                sops = sops_add(sops, op_r("JR NC", addr8));
                jmp_addr.push_back(phy(pc + ((char)addr8)));
                jmp(addr8);
                break;
            /* LD SP,d16 */
            case 0x31:
                addr16 = r->raw[phy(pc+1)] | (r->raw[phy(pc+2)]<<8);
                sops = sops_add(sops, op_r("LD SP", addr16));
                pc += 3;
                break;
            /* LD (HL-),A */
            case 0x32:
                sops = sops_add(sops, op_0("LD (HL-),A"));
                pc += 1;
                break;
            /* INC SP */
            case 0x33:
                sops = sops_add(sops, op_0("INC SP"));
                pc += 1;
                break;
            /* INC (HL) */
            case 0x34:
                sops = sops_add(sops, op_0("INC (HL)"));
                pc += 1;
                break;
            /* DEC (HL) */
            case 0x35:
                sops = sops_add(sops, op_0("DEC (HL)"));
                pc += 1;
                break;
            /* LD (HL),d8 */
            case 0x36:
                addr8 = r->raw[phy(pc+1)];
                sops = sops_add(sops, op_r("LD (HL)", addr8));
                pc += 2;
                break;
            /* SCF */
            case 0x37:
                sops = sops_add(sops, op_0("SCF"));
                pc += 1;
                break;
            /* JR C,r8 */
            case 0x38:
                addr8 = r->raw[phy(pc+1)];
                sops = sops_add(sops, op_r("JR C", addr8));
                jmp_addr.push_back(phy(pc + ((char)addr8)));
                jmp(addr8);
                break;
            /* ADD HL,SP */
            case 0x39:
                sops = sops_add(sops, op_0("ADD HL,SP"));
                pc += 1;
                break;
            /* LD A,(HL-) */
            case 0x3a:
                sops = sops_add(sops, op_0("LD A,(HL-)"));
                pc += 1;
                break;
            /* DEC SP */
            case 0x3b:
                sops = sops_add(sops, op_0("DEC SP"));
                pc += 1;
                break;
            /* INC A */
            case 0x3c:
                sops = sops_add(sops, op_0("INC A"));
                pc += 1;
                break;
            /* DEC A */
            case 0x3d:
                sops = sops_add(sops, op_0("DEC A"));
                pc += 1;
                break;
            /* LD A,d8 */
            case 0x3e:
                addr8 = r->raw[phy(pc+1)];
                sops = sops_add(sops, op_r("LD A", addr8));
                a = addr8;
                pc += 2;
                break;
            /* CCF */
            case 0x3f:
                sops = sops_add(sops, op_0("CCF"));
                pc += 1;
                break;
            /* LD B,B */
            case 0x40:
                sops = sops_add(sops, op_0("LD B,B"));
                pc += 1;
                break;
            /* LD B,C */
            case 0x41:
                sops = sops_add(sops, op_0("LD B,C"));
                pc += 1;
                break;
            /* LD B,D */
            case 0x42:
                sops = sops_add(sops, op_0("LD B,D"));
                pc += 1;
                break;
            /* LD B,E */
            case 0x43:
                sops = sops_add(sops, op_0("LD B,E"));
                pc += 1;
                break;
            /* LD B,H */
            case 0x44:
                sops = sops_add(sops, op_0("LD B,H"));
                pc += 1;
                break;
            /* LD B,L */
            case 0x45:
                sops = sops_add(sops, op_0("LD B,L"));
                pc += 1;
                break;
            /* LD B,(HL) */
            case 0x46:
                sops = sops_add(sops, op_0("LD B,(HL)"));
                pc += 1;
                break;
            /* LD B,A */
            case 0x47:
                sops = sops_add(sops, op_0("LD B,A"));
                pc += 1;
                break;
            /* LD C,B */
            case 0x48:
                sops = sops_add(sops, op_0("LD C,B"));
                pc += 1;
                break;
            /* LD C,C */
            case 0x49:
                sops = sops_add(sops, op_0("LD C,C"));
                pc += 1;
                break;
            /* LD C,D */
            case 0x4a:
                sops = sops_add(sops, op_0("LD C,D"));
                pc += 1;
                break;
            /* LD C,E */
            case 0x4b:
                sops = sops_add(sops, op_0("LD C,E"));
                pc += 1;
                break;
            /* LD C,H */
            case 0x4c:
                sops = sops_add(sops, op_0("LD C,H"));
                pc += 1;
                break;
            /* LD C,L */
            case 0x4d:
                sops = sops_add(sops, op_0("LD C,L"));
                pc += 1;
                break;
            /* LD C,(HL) */
            case 0x4e:
                sops = sops_add(sops, op_0("LD C,(HL)"));
                pc += 1;
                break;
            /* LD C,A */
            case 0x4f:
                sops = sops_add(sops, op_0("LD C,A"));
                pc += 1;
                break;
            /* LD D,B */
            case 0x50:
                sops = sops_add(sops, op_0("LD D,B"));
                pc += 1;
                break;
            /* LD D,C */
            case 0x51:
                sops = sops_add(sops, op_0("LD D,C"));
                pc += 1;
                break;
            /* LD D,D */
            case 0x52:
                sops = sops_add(sops, op_0("LD D,D"));
                pc += 1;
                break;
            /* LD D,E */
            case 0x53:
                sops = sops_add(sops, op_0("LD D,E"));
                pc += 1;
                break;
            /* LD D,H */
            case 0x54:
                sops = sops_add(sops, op_0("LD D,H"));
                pc += 1;
                break;
            /* LD D,L */
            case 0x55:
                sops = sops_add(sops, op_0("LD D,L"));
                pc += 1;
                break;
            /* LD D,(HL) */
            case 0x56:
                sops = sops_add(sops, op_0("LD D,(HL)"));
                pc += 1;
                break;
            /* LD D,A */
            case 0x57:
                sops = sops_add(sops, op_0("LD D,A"));
                pc += 1;
                break;
            /* LD E,B */
            case 0x58:
                sops = sops_add(sops, op_0("LD E,B"));
                pc += 1;
                break;
            /* LD E,C */
            case 0x59:
                sops = sops_add(sops, op_0("LD E,C"));
                pc += 1;
                break;
            /* LD E,D */
            case 0x5a:
                sops = sops_add(sops, op_0("LD E,D"));
                pc += 1;
                break;
            /* LD E,E */
            case 0x5b:
                sops = sops_add(sops, op_0("LD E,E"));
                pc += 1;
                break;
            /* LD E,H */
            case 0x5c:
                sops = sops_add(sops, op_0("LD E,H"));
                pc += 1;
                break;
            /* LD E,L */
            case 0x5d:
                sops = sops_add(sops, op_0("LD E,L"));
                pc += 1;
                break;
            /* LD E,(HL) */
            case 0x5e:
                sops = sops_add(sops, op_0("LD E,(HL)"));
                pc += 1;
                break;
            /* LD E,A */
            case 0x5f:
                sops = sops_add(sops, op_0("LD E,A"));
                pc += 1;
                break;
            /* LD H,B */
            case 0x60:
                sops = sops_add(sops, op_0("LD H,B"));
                pc += 1;
                break;
            /* LD H,C */
            case 0x61:
                sops = sops_add(sops, op_0("LD H,C"));
                pc += 1;
                break;
            /* LD H,D */
            case 0x62:
                sops = sops_add(sops, op_0("LD H,D"));
                pc += 1;
                break;
            /* LD H,E */
            case 0x63:
                sops = sops_add(sops, op_0("LD H,E"));
                pc += 1;
                break;
            /* LD H,H */
            case 0x64:
                sops = sops_add(sops, op_0("LD H,H"));
                pc += 1;
                break;
            /* LD H,L */
            case 0x65:
                sops = sops_add(sops, op_0("LD H,L"));
                pc += 1;
                break;
            /* LD H,(HL) */
            case 0x66:
                sops = sops_add(sops, op_0("LD H,(HL)"));
                pc += 1;
                break;
            /* LD H,A */
            case 0x67:
                sops = sops_add(sops, op_0("LD H,A"));
                pc += 1;
                break;
            /* LD L,B */
            case 0x68:
                sops = sops_add(sops, op_0("LD L,B"));
                pc += 1;
                break;
            /* LD L,C */
            case 0x69:
                sops = sops_add(sops, op_0("LD L,C"));
                pc += 1;
                break;
            /* LD L,D */
            case 0x6a:
                sops = sops_add(sops, op_0("LD L,D"));
                pc += 1;
                break;
            /* LD L,E */
            case 0x6b:
                sops = sops_add(sops, op_0("LD L,E"));
                pc += 1;
                break;
            /* LD L,H */
            case 0x6c:
                sops = sops_add(sops, op_0("LD L,H"));
                pc += 1;
                break;
            /* LD L,L */
            case 0x6d:
                sops = sops_add(sops, op_0("LD L,L"));
                pc += 1;
                break;
            /* LD L,(HL) */
            case 0x6e:
                sops = sops_add(sops, op_0("LD L,(HL)"));
                pc += 1;
                break;
            /* LD L,A */
            case 0x6f:
                sops = sops_add(sops, op_0("LD L,A"));
                pc += 1;
                break;
            /* LD (HL),B */
            case 0x70:
                sops = sops_add(sops, op_0("LD (HL),B"));
                pc += 1;
                break;
            /* LD (HL),C */
            case 0x71:
                sops = sops_add(sops, op_0("LD (HL),C"));
                pc += 1;
                break;
            /* LD (HL),D */
            case 0x72:
                sops = sops_add(sops, op_0("LD (HL),D"));
                pc += 1;
                break;
            /* LD (HL),E */
            case 0x73:
                sops = sops_add(sops, op_0("LD (HL),E"));
                pc += 1;
                break;
            /* LD (HL),H */
            case 0x74:
                sops = sops_add(sops, op_0("LD (HL),H"));
                pc += 1;
                break;
            /* LD (HL),L */
            case 0x75:
                sops = sops_add(sops, op_0("LD (HL),L"));
                pc += 1;
                break;
            /* HALT */
            case 0x76:
                sops = sops_add(sops, op_0("HALT"));
                pc += 1;
                break;
            /* LD (HL),A */
            case 0x77:
                sops = sops_add(sops, op_0("LD (HL),A"));
                pc += 1;
                break;
            /* LD A,B */
            case 0x78:
                sops = sops_add(sops, op_0("LD A,B"));
                pc += 1;
                break;
            /* LD A,C */
            case 0x79:
                sops = sops_add(sops, op_0("LD A,C"));
                pc += 1;
                break;
            /* LD A,D */
            case 0x7a:
                sops = sops_add(sops, op_0("LD A,D"));
                pc += 1;
                break;
            /* LD A,E */
            case 0x7b:
                sops = sops_add(sops, op_0("LD A,E"));
                pc += 1;
                break;
            /* LD A,H */
            case 0x7c:
                sops = sops_add(sops, op_0("LD A,H"));
                pc += 1;
                break;
            /* LD A,L */
            case 0x7d:
                sops = sops_add(sops, op_0("LD A,L"));
                pc += 1;
                break;
            /* LD A,(HL) */
            case 0x7e:
                sops = sops_add(sops, op_0("LD A,(HL)"));
                pc += 1;
                break;
            /* LD A,A */
            case 0x7f:
                sops = sops_add(sops, op_0("LD A,A"));
                pc += 1;
                break;
            /* ADD A,B */
            case 0x80:
                sops = sops_add(sops, op_0("ADD A,B"));
                pc += 1;
                break;
            /* ADD A,C */
            case 0x81:
                sops = sops_add(sops, op_0("ADD A,C"));
                pc += 1;
                break;
            /* ADD A,D */
            case 0x82:
                sops = sops_add(sops, op_0("ADD A,D"));
                pc += 1;
                break;
            /* ADD A,E */
            case 0x83:
                sops = sops_add(sops, op_0("ADD A,E"));
                pc += 1;
                break;
            /* ADD A,H */
            case 0x84:
                sops = sops_add(sops, op_0("ADD A,H"));
                pc += 1;
                break;
            /* ADD A,L */
            case 0x85:
                sops = sops_add(sops, op_0("ADD A,L"));
                pc += 1;
                break;
            /* ADD A,(HL) */
            case 0x86:
                sops = sops_add(sops, op_0("ADD A,(HL)"));
                pc += 1;
                break;
            /* ADD A,A */
            case 0x87:
                sops = sops_add(sops, op_0("ADD A,A"));
                pc += 1;
                break;
            /* ADC A,B */
            case 0x88:
                sops = sops_add(sops, op_0("ADC A,B"));
                pc += 1;
                break;
            /* ADC A,C */
            case 0x89:
                sops = sops_add(sops, op_0("ADC A,C"));
                pc += 1;
                break;
            /* ADC A,D */
            case 0x8a:
                sops = sops_add(sops, op_0("ADC A,D"));
                pc += 1;
                break;
            /* ADC A,E */
            case 0x8b:
                sops = sops_add(sops, op_0("ADC A,E"));
                pc += 1;
                break;
            /* ADC A,H */
            case 0x8c:
                sops = sops_add(sops, op_0("ADC A,H"));
                pc += 1;
                break;
            /* ADC A,L */
            case 0x8d:
                sops = sops_add(sops, op_0("ADC A,L"));
                pc += 1;
                break;
            /* ADC A,(HL) */
            case 0x8e:
                sops = sops_add(sops, op_0("ADC A,(HL)"));
                pc += 1;
                break;
            /* ADC A,A */
            case 0x8f:
                sops = sops_add(sops, op_0("ADC A,A"));
                pc += 1;
                break;
            /* SUB B */
            case 0x90:
                sops = sops_add(sops, op_0("SUB B"));
                pc += 1;
                break;
            /* SUB C */
            case 0x91:
                sops = sops_add(sops, op_0("SUB C"));
                pc += 1;
                break;
            /* SUB D */
            case 0x92:
                sops = sops_add(sops, op_0("SUB D"));
                pc += 1;
                break;
            /* SUB E */
            case 0x93:
                sops = sops_add(sops, op_0("SUB E"));
                pc += 1;
                break;
            /* SUB H */
            case 0x94:
                sops = sops_add(sops, op_0("SUB H"));
                pc += 1;
                break;
            /* SUB L */
            case 0x95:
                sops = sops_add(sops, op_0("SUB L"));
                pc += 1;
                break;
            /* SUB (HL) */
            case 0x96:
                sops = sops_add(sops, op_0("SUB (HL)"));
                pc += 1;
                break;
            /* SUB A */
            case 0x97:
                sops = sops_add(sops, op_0("SUB A"));
                pc += 1;
                break;
            /* SBC A,B */
            case 0x98:
                sops = sops_add(sops, op_0("SBC A,B"));
                pc += 1;
                break;
            /* SBC A,C */
            case 0x99:
                sops = sops_add(sops, op_0("SBC A,C"));
                pc += 1;
                break;
            /* SBC A,D */
            case 0x9a:
                sops = sops_add(sops, op_0("SBC A,D"));
                pc += 1;
                break;
            /* SBC A,E */
            case 0x9b:
                sops = sops_add(sops, op_0("SBC A,E"));
                pc += 1;
                break;
            /* SBC A,H */
            case 0x9c:
                sops = sops_add(sops, op_0("SBC A,H"));
                pc += 1;
                break;
            /* SBC A,L */
            case 0x9d:
                sops = sops_add(sops, op_0("SBC A,L"));
                pc += 1;
                break;
            /* SBC A,(HL) */
            case 0x9e:
                sops = sops_add(sops, op_0("SBC A,(HL)"));
                pc += 1;
                break;
            /* SBC A,A */
            case 0x9f:
                sops = sops_add(sops, op_0("SBC A,A"));
                pc += 1;
                break;
            /* AND B */
            case 0xa0:
                sops = sops_add(sops, op_0("AND B"));
                pc += 1;
                break;
            /* AND C */
            case 0xa1:
                sops = sops_add(sops, op_0("AND C"));
                pc += 1;
                break;
            /* AND D */
            case 0xa2:
                sops = sops_add(sops, op_0("AND D"));
                pc += 1;
                break;
            /* AND E */
            case 0xa3:
                sops = sops_add(sops, op_0("AND E"));
                pc += 1;
                break;
            /* AND H */
            case 0xa4:
                sops = sops_add(sops, op_0("AND H"));
                pc += 1;
                break;
            /* AND L */
            case 0xa5:
                sops = sops_add(sops, op_0("AND L"));
                pc += 1;
                break;
            /* AND (HL) */
            case 0xa6:
                sops = sops_add(sops, op_0("AND (HL)"));
                pc += 1;
                break;
            /* AND A */
            case 0xa7:
                sops = sops_add(sops, op_0("AND A"));
                pc += 1;
                break;
            /* XOR B */
            case 0xa8:
                sops = sops_add(sops, op_0("XOR B"));
                pc += 1;
                break;
            /* XOR C */
            case 0xa9:
                sops = sops_add(sops, op_0("XOR C"));
                pc += 1;
                break;
            /* XOR D */
            case 0xaa:
                sops = sops_add(sops, op_0("XOR D"));
                pc += 1;
                break;
            /* XOR E */
            case 0xab:
                sops = sops_add(sops, op_0("XOR E"));
                pc += 1;
                break;
            /* XOR H */
            case 0xac:
                sops = sops_add(sops, op_0("XOR H"));
                pc += 1;
                break;
            /* XOR L */
            case 0xad:
                sops = sops_add(sops, op_0("XOR L"));
                pc += 1;
                break;
            /* XOR (HL) */
            case 0xae:
                sops = sops_add(sops, op_0("XOR (HL)"));
                pc += 1;
                break;
            /* XOR A */
            case 0xaf:
                sops = sops_add(sops, op_0("XOR A"));
                pc += 1;
                break;
            /* OR B */
            case 0xb0:
                sops = sops_add(sops, op_0("OR B"));
                pc += 1;
                break;
            /* OR C */
            case 0xb1:
                sops = sops_add(sops, op_0("OR C"));
                pc += 1;
                break;
            /* OR D */
            case 0xb2:
                sops = sops_add(sops, op_0("OR D"));
                pc += 1;
                break;
            /* OR E */
            case 0xb3:
                sops = sops_add(sops, op_0("OR E"));
                pc += 1;
                break;
            /* OR H */
            case 0xb4:
                sops = sops_add(sops, op_0("OR H"));
                pc += 1;
                break;
            /* OR L */
            case 0xb5:
                sops = sops_add(sops, op_0("OR L"));
                pc += 1;
                break;
            /* OR (HL) */
            case 0xb6:
                sops = sops_add(sops, op_0("OR (HL)"));
                pc += 1;
                break;
            /* OR A */
            case 0xb7:
                sops = sops_add(sops, op_0("OR A"));
                pc += 1;
                break;
            /* CP B */
            case 0xb8:
                sops = sops_add(sops, op_0("CP B"));
                pc += 1;
                break;
            /* CP C */
            case 0xb9:
                sops = sops_add(sops, op_0("CP C"));
                pc += 1;
                break;
            /* CP D */
            case 0xba:
                sops = sops_add(sops, op_0("CP D"));
                pc += 1;
                break;
            /* CP E */
            case 0xbb:
                sops = sops_add(sops, op_0("CP E"));
                pc += 1;
                break;
            /* CP H */
            case 0xbc:
                sops = sops_add(sops, op_0("CP H"));
                pc += 1;
                break;
            /* CP L */
            case 0xbd:
                sops = sops_add(sops, op_0("CP L"));
                pc += 1;
                break;
            /* CP (HL) */
            case 0xbe:
                sops = sops_add(sops, op_0("CP (HL)"));
                pc += 1;
                break;
            /* CP A */
            case 0xbf:
                sops = sops_add(sops, op_0("CP A"));
                pc += 1;
                break;
            /* RET NZ */
            case 0xc0:
                sops = sops_add(sops, op_0("RET NZ"));
                pc += 1;
                break;
            /* POP BC */
            case 0xc1:
                sops = sops_add(sops, op_0("POP BC"));
                pc += 1;
                break;
            /* JP NZ,a16 */
            case 0xc2:
                addr16 = r->raw[phy(pc+1)] | (r->raw[phy(pc+2)]<<8);
                sops = sops_add(sops, op_r("JP NZ", addr16));
                jmp_addr.push_back(phy(addr16));
                jmp(addr16);
                break;
            /* JP a16 */
            case 0xc3:
                addr16 = r->raw[phy(pc+1)] | (r->raw[phy(pc+2)]<<8);
                sops = sops_add(sops, op_r("JP", addr16));
                jmp_addr.push_back(phy(addr16));
                jmpu(addr16);
                break;
            /* CALL NZ,a16 */
            case 0xc4:
                addr16 = r->raw[phy(pc+1)] | (r->raw[phy(pc+2)]<<8);
                sops = sops_add(sops, op_r("CALL NZ", addr16));
                call_addr.push_back(phy(addr16));
                jmp(addr16);
                break;
            /* PUSH BC */
            case 0xc5:
                sops = sops_add(sops, op_0("PUSH BC"));
                pc += 1;
                break;
            /* ADD A,d8 */
            case 0xc6:
                addr8 = r->raw[phy(pc+1)];
                sops = sops_add(sops, op_r("ADD A", addr8));
                pc += 2;
                break;
            /* RST 00H */
            case 0xc7:
                sops = sops_add(sops, op_0("RST 00H"));
                pc += 1;
                break;
            /* RET Z */
            case 0xc8:
                sops = sops_add(sops, op_0("RET Z"));
                pc += 1;
                break;
            /* RET */
            case 0xc9:
                sops = sops_add(sops, op_0("RET"));
                ret();
                break;
            /* JP Z,a16 */
            case 0xca:
                addr16 = r->raw[phy(pc+1)] | (r->raw[phy(pc+2)]<<8);
                sops = sops_add(sops, op_r("JP Z", addr16));
                jmp_addr.push_back(phy(addr16));
                jmp(addr16);
                break;
            /* bit operations */
            case 0xcb:
                switch(r->raw[phy(pc+1)]) {
                /* RLC B */
                case 0x0:
	                sops = sops_add(sops, op_0_2("RLC B"));
	                break;
                /* RLC C */
                case 0x1:
	                sops = sops_add(sops, op_0_2("RLC C"));
	                break;
                /* RLC D */
                case 0x2:
	                sops = sops_add(sops, op_0_2("RLC D"));
	                break;
                /* RLC E */
                case 0x3:
	                sops = sops_add(sops, op_0_2("RLC E"));
	                break;
                /* RLC H */
                case 0x4:
	                sops = sops_add(sops, op_0_2("RLC H"));
	                break;
                /* RLC L */
                case 0x5:
	                sops = sops_add(sops, op_0_2("RLC L"));
	                break;
                /* RLC (HL) */
                case 0x6:
	                sops = sops_add(sops, op_0_2("RLC (HL)"));
	                break;
                /* RLC A */
                case 0x7:
	                sops = sops_add(sops, op_0_2("RLC A"));
	                break;
                /* RRC B */
                case 0x8:
	                sops = sops_add(sops, op_0_2("RRC B"));
	                break;
                /* RRC C */
                case 0x9:
	                sops = sops_add(sops, op_0_2("RRC C"));
	                break;
                /* RRC D */
                case 0xa:
	                sops = sops_add(sops, op_0_2("RRC D"));
	                break;
                /* RRC E */
                case 0xb:
	                sops = sops_add(sops, op_0_2("RRC E"));
	                break;
                /* RRC H */
                case 0xc:
	                sops = sops_add(sops, op_0_2("RRC H"));
	                break;
                /* RRC L */
                case 0xd:
	                sops = sops_add(sops, op_0_2("RRC L"));
	                break;
                /* RRC (HL) */
                case 0xe:
	                sops = sops_add(sops, op_0_2("RRC (HL)"));
	                break;
                /* RRC A */
                case 0xf:
	                sops = sops_add(sops, op_0_2("RRC A"));
	                break;
                /* RL B */
                case 0x10:
	                sops = sops_add(sops, op_0_2("RL B"));
	                break;
                /* RL C */
                case 0x11:
	                sops = sops_add(sops, op_0_2("RL C"));
	                break;
                /* RL D */
                case 0x12:
	                sops = sops_add(sops, op_0_2("RL D"));
	                break;
                /* RL E */
                case 0x13:
	                sops = sops_add(sops, op_0_2("RL E"));
	                break;
                /* RL H */
                case 0x14:
	                sops = sops_add(sops, op_0_2("RL H"));
	                break;
                /* RL L */
                case 0x15:
	                sops = sops_add(sops, op_0_2("RL L"));
	                break;
                /* RL (HL) */
                case 0x16:
	                sops = sops_add(sops, op_0_2("RL (HL)"));
	                break;
                /* RL A */
                case 0x17:
	                sops = sops_add(sops, op_0_2("RL A"));
	                break;
                /* RR B */
                case 0x18:
	                sops = sops_add(sops, op_0_2("RR B"));
	                break;
                /* RR C */
                case 0x19:
	                sops = sops_add(sops, op_0_2("RR C"));
	                break;
                /* RR D */
                case 0x1a:
	                sops = sops_add(sops, op_0_2("RR D"));
	                break;
                /* RR E */
                case 0x1b:
	                sops = sops_add(sops, op_0_2("RR E"));
	                break;
                /* RR H */
                case 0x1c:
	                sops = sops_add(sops, op_0_2("RR H"));
	                break;
                /* RR L */
                case 0x1d:
	                sops = sops_add(sops, op_0_2("RR L"));
	                break;
                /* RR (HL) */
                case 0x1e:
	                sops = sops_add(sops, op_0_2("RR (HL)"));
	                break;
                /* RR A */
                case 0x1f:
	                sops = sops_add(sops, op_0_2("RR A"));
	                break;
                /* SLA B */
                case 0x20:
	                sops = sops_add(sops, op_0_2("SLA B"));
	                break;
                /* SLA C */
                case 0x21:
	                sops = sops_add(sops, op_0_2("SLA C"));
	                break;
                /* SLA D */
                case 0x22:
	                sops = sops_add(sops, op_0_2("SLA D"));
	                break;
                /* SLA E */
                case 0x23:
	                sops = sops_add(sops, op_0_2("SLA E"));
	                break;
                /* SLA H */
                case 0x24:
	                sops = sops_add(sops, op_0_2("SLA H"));
	                break;
                /* SLA L */
                case 0x25:
	                sops = sops_add(sops, op_0_2("SLA L"));
	                break;
                /* SLA (HL) */
                case 0x26:
	                sops = sops_add(sops, op_0_2("SLA (HL)"));
	                break;
                /* SLA A */
                case 0x27:
	                sops = sops_add(sops, op_0_2("SLA A"));
	                break;
                /* SRA B */
                case 0x28:
	                sops = sops_add(sops, op_0_2("SRA B"));
	                break;
                /* SRA C */
                case 0x29:
	                sops = sops_add(sops, op_0_2("SRA C"));
	                break;
                /* SRA D */
                case 0x2a:
	                sops = sops_add(sops, op_0_2("SRA D"));
	                break;
                /* SRA E */
                case 0x2b:
	                sops = sops_add(sops, op_0_2("SRA E"));
	                break;
                /* SRA H */
                case 0x2c:
	                sops = sops_add(sops, op_0_2("SRA H"));
	                break;
                /* SRA L */
                case 0x2d:
	                sops = sops_add(sops, op_0_2("SRA L"));
	                break;
                /* SRA (HL) */
                case 0x2e:
	                sops = sops_add(sops, op_0_2("SRA (HL)"));
	                break;
                /* SRA A */
                case 0x2f:
	                sops = sops_add(sops, op_0_2("SRA A"));
	                break;
                /* SWAP B */
                case 0x30:
	                sops = sops_add(sops, op_0_2("SWAP B"));
	                break;
                /* SWAP C */
                case 0x31:
	                sops = sops_add(sops, op_0_2("SWAP C"));
	                break;
                /* SWAP D */
                case 0x32:
	                sops = sops_add(sops, op_0_2("SWAP D"));
	                break;
                /* SWAP E */
                case 0x33:
	                sops = sops_add(sops, op_0_2("SWAP E"));
	                break;
                /* SWAP H */
                case 0x34:
	                sops = sops_add(sops, op_0_2("SWAP H"));
	                break;
                /* SWAP L */
                case 0x35:
	                sops = sops_add(sops, op_0_2("SWAP L"));
	                break;
                /* SWAP (HL) */
                case 0x36:
	                sops = sops_add(sops, op_0_2("SWAP (HL)"));
	                break;
                /* SWAP A */
                case 0x37:
	                sops = sops_add(sops, op_0_2("SWAP A"));
	                break;
                /* SRL B */
                case 0x38:
	                sops = sops_add(sops, op_0_2("SRL B"));
	                break;
                /* SRL C */
                case 0x39:
	                sops = sops_add(sops, op_0_2("SRL C"));
	                break;
                /* SRL D */
                case 0x3a:
	                sops = sops_add(sops, op_0_2("SRL D"));
	                break;
                /* SRL E */
                case 0x3b:
	                sops = sops_add(sops, op_0_2("SRL E"));
	                break;
                /* SRL H */
                case 0x3c:
	                sops = sops_add(sops, op_0_2("SRL H"));
	                break;
                /* SRL L */
                case 0x3d:
	                sops = sops_add(sops, op_0_2("SRL L"));
	                break;
                /* SRL (HL) */
                case 0x3e:
	                sops = sops_add(sops, op_0_2("SRL (HL)"));
	                break;
                /* SRL A */
                case 0x3f:
	                sops = sops_add(sops, op_0_2("SRL A"));
	                break;
                /* BIT 0,B */
                case 0x40:
	                sops = sops_add(sops, op_0_2("BIT 0,B"));
	                break;
                /* BIT 0,C */
                case 0x41:
	                sops = sops_add(sops, op_0_2("BIT 0,C"));
	                break;
                /* BIT 0,D */
                case 0x42:
	                sops = sops_add(sops, op_0_2("BIT 0,D"));
	                break;
                /* BIT 0,E */
                case 0x43:
	                sops = sops_add(sops, op_0_2("BIT 0,E"));
	                break;
                /* BIT 0,H */
                case 0x44:
	                sops = sops_add(sops, op_0_2("BIT 0,H"));
	                break;
                /* BIT 0,L */
                case 0x45:
	                sops = sops_add(sops, op_0_2("BIT 0,L"));
	                break;
                /* BIT 0,(HL) */
                case 0x46:
	                sops = sops_add(sops, op_0_2("BIT 0,(HL)"));
	                break;
                /* BIT 0,A */
                case 0x47:
	                sops = sops_add(sops, op_0_2("BIT 0,A"));
	                break;
                /* BIT 1,B */
                case 0x48:
	                sops = sops_add(sops, op_0_2("BIT 1,B"));
	                break;
                /* BIT 1,C */
                case 0x49:
	                sops = sops_add(sops, op_0_2("BIT 1,C"));
	                break;
                /* BIT 1,D */
                case 0x4a:
	                sops = sops_add(sops, op_0_2("BIT 1,D"));
	                break;
                /* BIT 1,E */
                case 0x4b:
	                sops = sops_add(sops, op_0_2("BIT 1,E"));
	                break;
                /* BIT 1,H */
                case 0x4c:
	                sops = sops_add(sops, op_0_2("BIT 1,H"));
	                break;
                /* BIT 1,L */
                case 0x4d:
	                sops = sops_add(sops, op_0_2("BIT 1,L"));
	                break;
                /* BIT 1,(HL) */
                case 0x4e:
	                sops = sops_add(sops, op_0_2("BIT 1,(HL)"));
	                break;
                /* BIT 1,A */
                case 0x4f:
	                sops = sops_add(sops, op_0_2("BIT 1,A"));
	                break;
                /* BIT 2,B */
                case 0x50:
	                sops = sops_add(sops, op_0_2("BIT 2,B"));
	                break;
                /* BIT 2,C */
                case 0x51:
	                sops = sops_add(sops, op_0_2("BIT 2,C"));
	                break;
                /* BIT 2,D */
                case 0x52:
	                sops = sops_add(sops, op_0_2("BIT 2,D"));
	                break;
                /* BIT 2,E */
                case 0x53:
	                sops = sops_add(sops, op_0_2("BIT 2,E"));
	                break;
                /* BIT 2,H */
                case 0x54:
	                sops = sops_add(sops, op_0_2("BIT 2,H"));
	                break;
                /* BIT 2,L */
                case 0x55:
	                sops = sops_add(sops, op_0_2("BIT 2,L"));
	                break;
                /* BIT 2,(HL) */
                case 0x56:
	                sops = sops_add(sops, op_0_2("BIT 2,(HL)"));
	                break;
                /* BIT 2,A */
                case 0x57:
	                sops = sops_add(sops, op_0_2("BIT 2,A"));
	                break;
                /* BIT 3,B */
                case 0x58:
	                sops = sops_add(sops, op_0_2("BIT 3,B"));
	                break;
                /* BIT 3,C */
                case 0x59:
	                sops = sops_add(sops, op_0_2("BIT 3,C"));
	                break;
                /* BIT 3,D */
                case 0x5a:
	                sops = sops_add(sops, op_0_2("BIT 3,D"));
	                break;
                /* BIT 3,E */
                case 0x5b:
	                sops = sops_add(sops, op_0_2("BIT 3,E"));
	                break;
                /* BIT 3,H */
                case 0x5c:
	                sops = sops_add(sops, op_0_2("BIT 3,H"));
	                break;
                /* BIT 3,L */
                case 0x5d:
	                sops = sops_add(sops, op_0_2("BIT 3,L"));
	                break;
                /* BIT 3,(HL) */
                case 0x5e:
	                sops = sops_add(sops, op_0_2("BIT 3,(HL)"));
	                break;
                /* BIT 3,A */
                case 0x5f:
	                sops = sops_add(sops, op_0_2("BIT 3,A"));
	                break;
                /* BIT 4,B */
                case 0x60:
	                sops = sops_add(sops, op_0_2("BIT 4,B"));
	                break;
                /* BIT 4,C */
                case 0x61:
	                sops = sops_add(sops, op_0_2("BIT 4,C"));
	                break;
                /* BIT 4,D */
                case 0x62:
	                sops = sops_add(sops, op_0_2("BIT 4,D"));
	                break;
                /* BIT 4,E */
                case 0x63:
	                sops = sops_add(sops, op_0_2("BIT 4,E"));
	                break;
                /* BIT 4,H */
                case 0x64:
	                sops = sops_add(sops, op_0_2("BIT 4,H"));
	                break;
                /* BIT 4,L */
                case 0x65:
	                sops = sops_add(sops, op_0_2("BIT 4,L"));
	                break;
                /* BIT 4,(HL) */
                case 0x66:
	                sops = sops_add(sops, op_0_2("BIT 4,(HL)"));
	                break;
                /* BIT 4,A */
                case 0x67:
	                sops = sops_add(sops, op_0_2("BIT 4,A"));
	                break;
                /* BIT 5,B */
                case 0x68:
	                sops = sops_add(sops, op_0_2("BIT 5,B"));
	                break;
                /* BIT 5,C */
                case 0x69:
	                sops = sops_add(sops, op_0_2("BIT 5,C"));
	                break;
                /* BIT 5,D */
                case 0x6a:
	                sops = sops_add(sops, op_0_2("BIT 5,D"));
	                break;
                /* BIT 5,E */
                case 0x6b:
	                sops = sops_add(sops, op_0_2("BIT 5,E"));
	                break;
                /* BIT 5,H */
                case 0x6c:
	                sops = sops_add(sops, op_0_2("BIT 5,H"));
	                break;
                /* BIT 5,L */
                case 0x6d:
	                sops = sops_add(sops, op_0_2("BIT 5,L"));
	                break;
                /* BIT 5,(HL) */
                case 0x6e:
	                sops = sops_add(sops, op_0_2("BIT 5,(HL)"));
	                break;
                /* BIT 5,A */
                case 0x6f:
	                sops = sops_add(sops, op_0_2("BIT 5,A"));
	                break;
                /* BIT 6,B */
                case 0x70:
	                sops = sops_add(sops, op_0_2("BIT 6,B"));
	                break;
                /* BIT 6,C */
                case 0x71:
	                sops = sops_add(sops, op_0_2("BIT 6,C"));
	                break;
                /* BIT 6,D */
                case 0x72:
	                sops = sops_add(sops, op_0_2("BIT 6,D"));
	                break;
                /* BIT 6,E */
                case 0x73:
	                sops = sops_add(sops, op_0_2("BIT 6,E"));
	                break;
                /* BIT 6,H */
                case 0x74:
	                sops = sops_add(sops, op_0_2("BIT 6,H"));
	                break;
                /* BIT 6,L */
                case 0x75:
	                sops = sops_add(sops, op_0_2("BIT 6,L"));
	                break;
                /* BIT 6,(HL) */
                case 0x76:
	                sops = sops_add(sops, op_0_2("BIT 6,(HL)"));
	                break;
                /* BIT 6,A */
                case 0x77:
	                sops = sops_add(sops, op_0_2("BIT 6,A"));
	                break;
                /* BIT 7,B */
                case 0x78:
	                sops = sops_add(sops, op_0_2("BIT 7,B"));
	                break;
                /* BIT 7,C */
                case 0x79:
	                sops = sops_add(sops, op_0_2("BIT 7,C"));
	                break;
                /* BIT 7,D */
                case 0x7a:
	                sops = sops_add(sops, op_0_2("BIT 7,D"));
	                break;
                /* BIT 7,E */
                case 0x7b:
	                sops = sops_add(sops, op_0_2("BIT 7,E"));
	                break;
                /* BIT 7,H */
                case 0x7c:
	                sops = sops_add(sops, op_0_2("BIT 7,H"));
	                break;
                /* BIT 7,L */
                case 0x7d:
	                sops = sops_add(sops, op_0_2("BIT 7,L"));
	                break;
                /* BIT 7,(HL) */
                case 0x7e:
	                sops = sops_add(sops, op_0_2("BIT 7,(HL)"));
	                break;
                /* BIT 7,A */
                case 0x7f:
	                sops = sops_add(sops, op_0_2("BIT 7,A"));
	                break;
                /* RES 0,B */
                case 0x80:
	                sops = sops_add(sops, op_0_2("RES 0,B"));
	                break;
                /* RES 0,C */
                case 0x81:
	                sops = sops_add(sops, op_0_2("RES 0,C"));
	                break;
                /* RES 0,D */
                case 0x82:
	                sops = sops_add(sops, op_0_2("RES 0,D"));
	                break;
                /* RES 0,E */
                case 0x83:
	                sops = sops_add(sops, op_0_2("RES 0,E"));
	                break;
                /* RES 0,H */
                case 0x84:
	                sops = sops_add(sops, op_0_2("RES 0,H"));
	                break;
                /* RES 0,L */
                case 0x85:
	                sops = sops_add(sops, op_0_2("RES 0,L"));
	                break;
                /* RES 0,(HL) */
                case 0x86:
	                sops = sops_add(sops, op_0_2("RES 0,(HL)"));
	                break;
                /* RES 0,A */
                case 0x87:
	                sops = sops_add(sops, op_0_2("RES 0,A"));
	                break;
                /* RES 1,B */
                case 0x88:
	                sops = sops_add(sops, op_0_2("RES 1,B"));
	                break;
                /* RES 1,C */
                case 0x89:
	                sops = sops_add(sops, op_0_2("RES 1,C"));
	                break;
                /* RES 1,D */
                case 0x8a:
	                sops = sops_add(sops, op_0_2("RES 1,D"));
	                break;
                /* RES 1,E */
                case 0x8b:
	                sops = sops_add(sops, op_0_2("RES 1,E"));
	                break;
                /* RES 1,H */
                case 0x8c:
	                sops = sops_add(sops, op_0_2("RES 1,H"));
	                break;
                /* RES 1,L */
                case 0x8d:
	                sops = sops_add(sops, op_0_2("RES 1,L"));
	                break;
                /* RES 1,(HL) */
                case 0x8e:
	                sops = sops_add(sops, op_0_2("RES 1,(HL)"));
	                break;
                /* RES 1,A */
                case 0x8f:
	                sops = sops_add(sops, op_0_2("RES 1,A"));
	                break;
                /* RES 2,B */
                case 0x90:
	                sops = sops_add(sops, op_0_2("RES 2,B"));
	                break;
                /* RES 2,C */
                case 0x91:
	                sops = sops_add(sops, op_0_2("RES 2,C"));
	                break;
                /* RES 2,D */
                case 0x92:
	                sops = sops_add(sops, op_0_2("RES 2,D"));
	                break;
                /* RES 2,E */
                case 0x93:
	                sops = sops_add(sops, op_0_2("RES 2,E"));
	                break;
                /* RES 2,H */
                case 0x94:
	                sops = sops_add(sops, op_0_2("RES 2,H"));
	                break;
                /* RES 2,L */
                case 0x95:
	                sops = sops_add(sops, op_0_2("RES 2,L"));
	                break;
                /* RES 2,(HL) */
                case 0x96:
	                sops = sops_add(sops, op_0_2("RES 2,(HL)"));
	                break;
                /* RES 2,A */
                case 0x97:
	                sops = sops_add(sops, op_0_2("RES 2,A"));
	                break;
                /* RES 3,B */
                case 0x98:
	                sops = sops_add(sops, op_0_2("RES 3,B"));
	                break;
                /* RES 3,C */
                case 0x99:
	                sops = sops_add(sops, op_0_2("RES 3,C"));
	                break;
                /* RES 3,D */
                case 0x9a:
	                sops = sops_add(sops, op_0_2("RES 3,D"));
	                break;
                /* RES 3,E */
                case 0x9b:
	                sops = sops_add(sops, op_0_2("RES 3,E"));
	                break;
                /* RES 3,H */
                case 0x9c:
	                sops = sops_add(sops, op_0_2("RES 3,H"));
	                break;
                /* RES 3,L */
                case 0x9d:
	                sops = sops_add(sops, op_0_2("RES 3,L"));
	                break;
                /* RES 3,(HL) */
                case 0x9e:
	                sops = sops_add(sops, op_0_2("RES 3,(HL)"));
	                break;
                /* RES 3,A */
                case 0x9f:
	                sops = sops_add(sops, op_0_2("RES 3,A"));
	                break;
                /* RES 4,B */
                case 0xa0:
	                sops = sops_add(sops, op_0_2("RES 4,B"));
	                break;
                /* RES 4,C */
                case 0xa1:
	                sops = sops_add(sops, op_0_2("RES 4,C"));
	                break;
                /* RES 4,D */
                case 0xa2:
	                sops = sops_add(sops, op_0_2("RES 4,D"));
	                break;
                /* RES 4,E */
                case 0xa3:
	                sops = sops_add(sops, op_0_2("RES 4,E"));
	                break;
                /* RES 4,H */
                case 0xa4:
	                sops = sops_add(sops, op_0_2("RES 4,H"));
	                break;
                /* RES 4,L */
                case 0xa5:
	                sops = sops_add(sops, op_0_2("RES 4,L"));
	                break;
                /* RES 4,(HL) */
                case 0xa6:
	                sops = sops_add(sops, op_0_2("RES 4,(HL)"));
	                break;
                /* RES 4,A */
                case 0xa7:
	                sops = sops_add(sops, op_0_2("RES 4,A"));
	                break;
                /* RES 5,B */
                case 0xa8:
	                sops = sops_add(sops, op_0_2("RES 5,B"));
	                break;
                /* RES 5,C */
                case 0xa9:
	                sops = sops_add(sops, op_0_2("RES 5,C"));
	                break;
                /* RES 5,D */
                case 0xaa:
	                sops = sops_add(sops, op_0_2("RES 5,D"));
	                break;
                /* RES 5,E */
                case 0xab:
	                sops = sops_add(sops, op_0_2("RES 5,E"));
	                break;
                /* RES 5,H */
                case 0xac:
	                sops = sops_add(sops, op_0_2("RES 5,H"));
	                break;
                /* RES 5,L */
                case 0xad:
	                sops = sops_add(sops, op_0_2("RES 5,L"));
	                break;
                /* RES 5,(HL) */
                case 0xae:
	                sops = sops_add(sops, op_0_2("RES 5,(HL)"));
	                break;
                /* RES 5,A */
                case 0xaf:
	                sops = sops_add(sops, op_0_2("RES 5,A"));
	                break;
                /* RES 6,B */
                case 0xb0:
	                sops = sops_add(sops, op_0_2("RES 6,B"));
	                break;
                /* RES 6,C */
                case 0xb1:
	                sops = sops_add(sops, op_0_2("RES 6,C"));
	                break;
                /* RES 6,D */
                case 0xb2:
	                sops = sops_add(sops, op_0_2("RES 6,D"));
	                break;
                /* RES 6,E */
                case 0xb3:
	                sops = sops_add(sops, op_0_2("RES 6,E"));
	                break;
                /* RES 6,H */
                case 0xb4:
	                sops = sops_add(sops, op_0_2("RES 6,H"));
	                break;
                /* RES 6,L */
                case 0xb5:
	                sops = sops_add(sops, op_0_2("RES 6,L"));
	                break;
                /* RES 6,(HL) */
                case 0xb6:
	                sops = sops_add(sops, op_0_2("RES 6,(HL)"));
	                break;
                /* RES 6,A */
                case 0xb7:
	                sops = sops_add(sops, op_0_2("RES 6,A"));
	                break;
                /* RES 7,B */
                case 0xb8:
	                sops = sops_add(sops, op_0_2("RES 7,B"));
	                break;
                /* RES 7,C */
                case 0xb9:
	                sops = sops_add(sops, op_0_2("RES 7,C"));
	                break;
                /* RES 7,D */
                case 0xba:
	                sops = sops_add(sops, op_0_2("RES 7,D"));
	                break;
                /* RES 7,E */
                case 0xbb:
	                sops = sops_add(sops, op_0_2("RES 7,E"));
	                break;
                /* RES 7,H */
                case 0xbc:
	                sops = sops_add(sops, op_0_2("RES 7,H"));
	                break;
                /* RES 7,L */
                case 0xbd:
	                sops = sops_add(sops, op_0_2("RES 7,L"));
	                break;
                /* RES 7,(HL) */
                case 0xbe:
	                sops = sops_add(sops, op_0_2("RES 7,(HL)"));
	                break;
                /* RES 7,A */
                case 0xbf:
	                sops = sops_add(sops, op_0_2("RES 7,A"));
	                break;
                /* SET 0,B */
                case 0xc0:
	                sops = sops_add(sops, op_0_2("SET 0,B"));
	                break;
                /* SET 0,C */
                case 0xc1:
	                sops = sops_add(sops, op_0_2("SET 0,C"));
	                break;
                /* SET 0,D */
                case 0xc2:
	                sops = sops_add(sops, op_0_2("SET 0,D"));
	                break;
                /* SET 0,E */
                case 0xc3:
	                sops = sops_add(sops, op_0_2("SET 0,E"));
	                break;
                /* SET 0,H */
                case 0xc4:
	                sops = sops_add(sops, op_0_2("SET 0,H"));
	                break;
                /* SET 0,L */
                case 0xc5:
	                sops = sops_add(sops, op_0_2("SET 0,L"));
	                break;
                /* SET 0,(HL) */
                case 0xc6:
	                sops = sops_add(sops, op_0_2("SET 0,(HL)"));
	                break;
                /* SET 0,A */
                case 0xc7:
	                sops = sops_add(sops, op_0_2("SET 0,A"));
	                break;
                /* SET 1,B */
                case 0xc8:
	                sops = sops_add(sops, op_0_2("SET 1,B"));
	                break;
                /* SET 1,C */
                case 0xc9:
	                sops = sops_add(sops, op_0_2("SET 1,C"));
	                break;
                /* SET 1,D */
                case 0xca:
	                sops = sops_add(sops, op_0_2("SET 1,D"));
	                break;
                /* SET 1,E */
                case 0xcb:
	                sops = sops_add(sops, op_0_2("SET 1,E"));
	                break;
                /* SET 1,H */
                case 0xcc:
	                sops = sops_add(sops, op_0_2("SET 1,H"));
	                break;
                /* SET 1,L */
                case 0xcd:
	                sops = sops_add(sops, op_0_2("SET 1,L"));
	                break;
                /* SET 1,(HL) */
                case 0xce:
	                sops = sops_add(sops, op_0_2("SET 1,(HL)"));
	                break;
                /* SET 1,A */
                case 0xcf:
	                sops = sops_add(sops, op_0_2("SET 1,A"));
	                break;
                /* SET 2,B */
                case 0xd0:
	                sops = sops_add(sops, op_0_2("SET 2,B"));
	                break;
                /* SET 2,C */
                case 0xd1:
	                sops = sops_add(sops, op_0_2("SET 2,C"));
	                break;
                /* SET 2,D */
                case 0xd2:
	                sops = sops_add(sops, op_0_2("SET 2,D"));
	                break;
                /* SET 2,E */
                case 0xd3:
	                sops = sops_add(sops, op_0_2("SET 2,E"));
	                break;
                /* SET 2,H */
                case 0xd4:
	                sops = sops_add(sops, op_0_2("SET 2,H"));
	                break;
                /* SET 2,L */
                case 0xd5:
	                sops = sops_add(sops, op_0_2("SET 2,L"));
	                break;
                /* SET 2,(HL) */
                case 0xd6:
	                sops = sops_add(sops, op_0_2("SET 2,(HL)"));
	                break;
                /* SET 2,A */
                case 0xd7:
	                sops = sops_add(sops, op_0_2("SET 2,A"));
	                break;
                /* SET 3,B */
                case 0xd8:
	                sops = sops_add(sops, op_0_2("SET 3,B"));
	                break;
                /* SET 3,C */
                case 0xd9:
	                sops = sops_add(sops, op_0_2("SET 3,C"));
	                break;
                /* SET 3,D */
                case 0xda:
	                sops = sops_add(sops, op_0_2("SET 3,D"));
	                break;
                /* SET 3,E */
                case 0xdb:
	                sops = sops_add(sops, op_0_2("SET 3,E"));
	                break;
                /* SET 3,H */
                case 0xdc:
	                sops = sops_add(sops, op_0_2("SET 3,H"));
	                break;
                /* SET 3,L */
                case 0xdd:
	                sops = sops_add(sops, op_0_2("SET 3,L"));
	                break;
                /* SET 3,(HL) */
                case 0xde:
	                sops = sops_add(sops, op_0_2("SET 3,(HL)"));
	                break;
                /* SET 3,A */
                case 0xdf:
	                sops = sops_add(sops, op_0_2("SET 3,A"));
	                break;
                /* SET 4,B */
                case 0xe0:
	                sops = sops_add(sops, op_0_2("SET 4,B"));
	                break;
                /* SET 4,C */
                case 0xe1:
	                sops = sops_add(sops, op_0_2("SET 4,C"));
	                break;
                /* SET 4,D */
                case 0xe2:
	                sops = sops_add(sops, op_0_2("SET 4,D"));
	                break;
                /* SET 4,E */
                case 0xe3:
	                sops = sops_add(sops, op_0_2("SET 4,E"));
	                break;
                /* SET 4,H */
                case 0xe4:
	                sops = sops_add(sops, op_0_2("SET 4,H"));
	                break;
                /* SET 4,L */
                case 0xe5:
	                sops = sops_add(sops, op_0_2("SET 4,L"));
	                break;
                /* SET 4,(HL) */
                case 0xe6:
	                sops = sops_add(sops, op_0_2("SET 4,(HL)"));
	                break;
                /* SET 4,A */
                case 0xe7:
	                sops = sops_add(sops, op_0_2("SET 4,A"));
	                break;
                /* SET 5,B */
                case 0xe8:
	                sops = sops_add(sops, op_0_2("SET 5,B"));
	                break;
                /* SET 5,C */
                case 0xe9:
	                sops = sops_add(sops, op_0_2("SET 5,C"));
	                break;
                /* SET 5,D */
                case 0xea:
	                sops = sops_add(sops, op_0_2("SET 5,D"));
	                break;
                /* SET 5,E */
                case 0xeb:
	                sops = sops_add(sops, op_0_2("SET 5,E"));
	                break;
                /* SET 5,H */
                case 0xec:
	                sops = sops_add(sops, op_0_2("SET 5,H"));
	                break;
                /* SET 5,L */
                case 0xed:
	                sops = sops_add(sops, op_0_2("SET 5,L"));
	                break;
                /* SET 5,(HL) */
                case 0xee:
	                sops = sops_add(sops, op_0_2("SET 5,(HL)"));
	                break;
                /* SET 5,A */
                case 0xef:
	                sops = sops_add(sops, op_0_2("SET 5,A"));
	                break;
                /* SET 6,B */
                case 0xf0:
	                sops = sops_add(sops, op_0_2("SET 6,B"));
	                break;
                /* SET 6,C */
                case 0xf1:
	                sops = sops_add(sops, op_0_2("SET 6,C"));
	                break;
                /* SET 6,D */
                case 0xf2:
	                sops = sops_add(sops, op_0_2("SET 6,D"));
	                break;
                /* SET 6,E */
                case 0xf3:
	                sops = sops_add(sops, op_0_2("SET 6,E"));
	                break;
                /* SET 6,H */
                case 0xf4:
	                sops = sops_add(sops, op_0_2("SET 6,H"));
	                break;
                /* SET 6,L */
                case 0xf5:
	                sops = sops_add(sops, op_0_2("SET 6,L"));
	                break;
                /* SET 6,(HL) */
                case 0xf6:
	                sops = sops_add(sops, op_0_2("SET 6,(HL)"));
	                break;
                /* SET 6,A */
                case 0xf7:
	                sops = sops_add(sops, op_0_2("SET 6,A"));
	                break;
                /* SET 7,B */
                case 0xf8:
	                sops = sops_add(sops, op_0_2("SET 7,B"));
	                break;
                /* SET 7,C */
                case 0xf9:
	                sops = sops_add(sops, op_0_2("SET 7,C"));
	                break;
                /* SET 7,D */
                case 0xfa:
	                sops = sops_add(sops, op_0_2("SET 7,D"));
	                break;
                /* SET 7,E */
                case 0xfb:
	                sops = sops_add(sops, op_0_2("SET 7,E"));
	                break;
                /* SET 7,H */
                case 0xfc:
	                sops = sops_add(sops, op_0_2("SET 7,H"));
	                break;
                /* SET 7,L */
                case 0xfd:
	                sops = sops_add(sops, op_0_2("SET 7,L"));
	                break;
                /* SET 7,(HL) */
                case 0xfe:
	                sops = sops_add(sops, op_0_2("SET 7,(HL)"));
	                break;
                /* SET 7,A */
                case 0xff:
	                sops = sops_add(sops, op_0_2("SET 7,A"));
	                break;
                }
                pc += 2;
                break;
            /* CALL Z,a16 */
            case 0xcc:
                addr16 = r->raw[phy(pc+1)] | (r->raw[phy(pc+2)]<<8);
                sops = sops_add(sops, op_r("CALL Z", addr16));
                call_addr.push_back(phy(addr16));
                jmp(addr16);
                break;
            /* CALL a16 */
            case 0xcd:
                addr16 = r->raw[phy(pc+1)] | (r->raw[phy(pc+2)]<<8);
                sops = sops_add(sops, op_r("CALL", addr16));
                call_addr.push_back(phy(addr16));
                jmp(addr16);
                break;
            /* ADC A,d8 */
            case 0xce:
                addr8 = r->raw[phy(pc+1)];
                sops = sops_add(sops, op_r("ADC A", addr8));
                pc += 2;
                break;
            /* RST 08H */
            case 0xcf:
                sops = sops_add(sops, op_0("RST 08H"));
                pc += 1;
                break;
            /* RET NC */
            case 0xd0:
                sops = sops_add(sops, op_0("RET NC"));
                pc += 1;
                break;
            /* POP DE */
            case 0xd1:
                sops = sops_add(sops, op_0("POP DE"));
                pc += 1;
                break;
            /* JP NC,a16 */
            case 0xd2:
                addr16 = r->raw[phy(pc+1)] | (r->raw[phy(pc+2)]<<8);
                sops = sops_add(sops, op_r("JP NC", addr16));
                jmp_addr.push_back(phy(addr16));
                jmp(addr16);
                break;
            /* CALL NC,a16 */
            case 0xd4:
                addr16 = r->raw[phy(pc+1)] | (r->raw[phy(pc+2)]<<8);
                sops = sops_add(sops, op_r("CALL NC", addr16));
                call_addr.push_back(phy(addr16));
                jmp(addr16);
                break;
            /* PUSH DE */
            case 0xd5:
                sops = sops_add(sops, op_0("PUSH DE"));
                pc += 1;
                break;
            /* SUB d8 */
            case 0xd6:
                addr8 = r->raw[phy(pc+1)];
                sops = sops_add(sops, op_r("SUB", addr8));
                pc += 2;
                break;
            /* RST 10H */
            case 0xd7:
                sops = sops_add(sops, op_0("RST 10H"));
                pc += 1;
                break;
            /* RET C */
            case 0xd8:
                sops = sops_add(sops, op_0("RET C"));
                pc += 1;
                break;
            /* RETI */
            case 0xd9:
                sops = sops_add(sops, op_0("RETI"));
                ret();
                break;
            /* JP C,a16 */
            case 0xda:
                addr16 = r->raw[phy(pc+1)] | (r->raw[phy(pc+2)]<<8);
                sops = sops_add(sops, op_r("JP C", addr16));
                jmp_addr.push_back(phy(addr16));
                jmp(addr16);
                break;
            /* CALL C,a16 */
            case 0xdc:
                addr16 = r->raw[phy(pc+1)] | (r->raw[phy(pc+2)]<<8);
                sops = sops_add(sops, op_r("CALL C", addr16));
                call_addr.push_back(phy(addr16));
                jmp(addr16);
                break;
            /* SBC A,d8 */
            case 0xde:
                addr8 = r->raw[phy(pc+1)];
                sops = sops_add(sops, op_r("SBC A", addr8));
                pc += 2;
                break;
            /* RST 18H */
            case 0xdf:
                sops = sops_add(sops, op_0("RST 18H"));
                pc += 1;
                break;
            /* LDH (a8),A */
            case 0xe0:
                addr8 = r->raw[phy(pc+1)];
                sops = sops_add(sops, op_lb("LDH", addr8, ",A"));
                hmem[addr8] = a;
                pc += 2;
                break;
            /* POP HL */
            case 0xe1:
                sops = sops_add(sops, op_0("POP HL"));
                pc += 1;
                break;
            /* LD (C),A */
            case 0xe2:
                sops = sops_add(sops, op_0("LD (C),A"));
                pc += 1;
                break;
            /* PUSH HL */
            case 0xe5:
                sops = sops_add(sops, op_0("PUSH HL"));
                pc += 1;
                break;
            /* AND d8 */
            case 0xe6:
                addr8 = r->raw[phy(pc+1)];
                sops = sops_add(sops, op_r("AND", addr8));
                pc += 2;
                break;
            /* RST 20H */
            case 0xe7:
                sops = sops_add(sops, op_0("RST 20H"));
                pc += 1;
                break;
            /* ADD SP,r8 */
            case 0xe8:
                addr8 = r->raw[phy(pc+1)];
                sops = sops_add(sops, op_r("ADD SP", addr8));
                pc += 2;
                break;
            /* JP (HL) */
            case 0xe9:
                sops = sops_add(sops, op_0("JP (HL)"));
                jmp_addr.push_back(phy(addr16));
                jmp(addr16);
                break;
            /* LD (a16),A */
            case 0xea:
                addr16 = r->raw[phy(pc+1)] | (r->raw[phy(pc+2)]<<8);
                sops = sops_add(sops, op_lb("LD", addr16, ",A"));
                if(addr16 == 0x2000 || addr16 == 0x2100) {
	                printf("[0x%.8X] Bank switch to %d\n", phy(pc), bank);
	                bank = a;
                }
                pc += 3;
                break;
            /* XOR d8 */
            case 0xee:
                addr8 = r->raw[phy(pc+1)];
                sops = sops_add(sops, op_r("XOR", addr8));
                pc += 2;
                break;
            /* RST 28H */
            case 0xef:
                sops = sops_add(sops, op_0("RST 28H"));
                pc += 1;
                break;
            /* LDH A,(a8) */
            case 0xf0:
                addr8 = r->raw[phy(pc+1)];
                sops = sops_add(sops, op_rb("LDH A", addr8));
                a = hmem[addr8];
                pc += 2;
                break;
            /* POP AF */
            case 0xf1:
                sops = sops_add(sops, op_0("POP AF"));
                pc += 1;
                break;
            /* LD A,(C) */
            case 0xf2:
                sops = sops_add(sops, op_0("LD A,(C)"));
                pc += 1;
                break;
            /* DI */
            case 0xf3:
                sops = sops_add(sops, op_0("DI"));
                pc += 1;
                break;
            /* PUSH AF */
            case 0xf5:
                sops = sops_add(sops, op_0("PUSH AF"));
                pc += 1;
                break;
            /* OR d8 */
            case 0xf6:
                addr8 = r->raw[phy(pc+1)];
                sops = sops_add(sops, op_r("OR", addr8));
                pc += 2;
                break;
            /* RST 30H */
            case 0xf7:
                sops = sops_add(sops, op_0("RST 30H"));
                pc += 1;
                break;
            /* LD HL,SP+r8 */
            case 0xf8:
                addr8 = r->raw[phy(pc+1)];
                sops = sops_add(sops, op_r("LD HL,SP", addr8));
                pc += 2;
                break;
            /* LD SP,HL */
            case 0xf9:
                sops = sops_add(sops, op_0("LD SP,HL"));
                pc += 1;
                break;
            /* LD A,(a16) */
            case 0xfa:
                addr16 = r->raw[phy(pc+1)] | (r->raw[phy(pc+2)]<<8);
                sops = sops_add(sops, op_rb("LD A", addr16));
                pc += 3;
                break;
            /* EI */
            case 0xfb:
                sops = sops_add(sops, op_0("EI"));
                pc += 1;
                break;
            /* CP d8 */
            case 0xfe:
                addr8 = r->raw[phy(pc+1)];
                sops = sops_add(sops, op_r("CP", addr8));
                pc += 2;
                break;
            /* RST 38H */
            case 0xff:
                sops = sops_add(sops, op_0("RST 38H"));
                pc += 1;
                break;
            /* AUTOGENERATED - end */
			default:
				printf("[0x%.8X] Unknown opcode (0x%.2X), stopping\n", phy(pc), r->raw[phy(pc)]);
				goto finish;
		}
	}

finish:
	print_dump();

	rom_free(r);
	state_free(top);
	
	return 0;
}