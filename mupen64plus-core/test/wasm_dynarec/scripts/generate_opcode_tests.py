import os
import re
import subprocess

MIPS_DEF = os.path.join(os.path.dirname(__file__), '../../..', 'src/device/r4300/mips_instructions.def')

def parse_instruction_names():
    names = []
    pattern = re.compile(r"DECLARE_(?:INSTRUCTION|JUMP)\(([^)]+)\)")
    with open(MIPS_DEF, 'r') as f:
        for line in f:
            m = pattern.search(line)
            if m:
                name = m.group(1)
                if name not in names:
                    names.append(name)
    return names

# simple templates for a handful of opcodes
TEMPLATES = {
    'ADD': 'add $t0, $t0, $t1',
    'ADDI': 'addi $t0, $t0, 1',
    'ADDIU': 'addiu $t0, $t0, 1',
    'SUB': 'sub $t0, $t0, $t1',
    'AND': 'and $t0, $t0, $t1',
    'OR': 'or $t0, $t0, $t1',
    'XOR': 'xor $t0, $t0, $t1',
    'NOR': 'nor $t0, $t0, $t1',
    'SLL': 'sll $t0, $t0, 1',
    'SRL': 'srl $t0, $t0, 1',
    'SRA': 'sra $t0, $t0, 1',
    'LW':  'lw $t0, 0($t1)',
    'SW':  'sw $t0, 0($t1)',
    'J':   'j 0x80000004',
    'JAL': 'jal 0x80000004',
    'JR':  'jr $ra',
    'JALR':'jalr $ra, $t0',
    'BEQ': 'beq $t0, $t1, 1',
    'BNE': 'bne $t0, $t1, 1',
}

ASM_PREFIX = '.set noreorder\n.text\n.globl start\nstart:'

def assemble_instruction(name, asm):
    asm_code = f"{ASM_PREFIX}\n    {asm}\n    nop\n"
    path = os.path.join('generated', f'{name.lower()}.s')
    os.makedirs('generated', exist_ok=True)
    with open(path, 'w') as f:
        f.write(asm_code)
    obj = os.path.join('generated', f'{name.lower()}.o')
    binfile = os.path.join('generated', f'{name.lower()}.bin')
    subprocess.check_call(['mipsel-linux-gnu-as', path, '-o', obj])
    subprocess.check_call(['mipsel-linux-gnu-objcopy', '-O', 'binary', '-j', '.text', obj, binfile])
    with open(binfile, 'rb') as f:
        data = f.read()
    data = data[:8]
    words = [data[i:i+4] for i in range(0, len(data), 4)]
    hex_words = [hex(int.from_bytes(w, 'little')) for w in words]
    return hex_words

def main():
    names = parse_instruction_names()
    for name in names:
        if name in TEMPLATES:
            try:
                words = assemble_instruction(name, TEMPLATES[name])
                print(f"{name}: {', '.join(words)}")
            except subprocess.CalledProcessError:
                print(f"Failed to assemble {name}")
        else:
            print(f"{name}: [no template]")

if __name__ == '__main__':
    main()
