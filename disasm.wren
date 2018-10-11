
# Disassember code

let dis_pc = 0


fun dis_string = 
    dis_pc : (dis_pc + 1);
    if *(dis_pc-1) = 0 then 0
    else (putc *(dis_pc-1) ; dis_string)

# dis_value assumes wValue is 4 bytes
#
fun dis_value bytes =
    if      bytes = 1 then (dis_pc : (dis_pc + 1);    * (dis_pc - 1))
    else if bytes = 2 then (dis_pc : (dis_pc + 2); refx (dis_pc - 2))
    else if bytes = 4 then (dis_pc : (dis_pc + 4); refv (dis_pc - 4))
    else 0

fun dis_fun_lookup addr hdr =
    if hdr < d0 then (
        if addr = (refx hdr) then
            put_name hdr
        else
            dis_fun_lookup addr (next_hdr hdr))
    else (  # didn't find it, so just print address 
        puts '0x'; putx addr)

fun dis_call_arity addr = *(c0 + addr)

fun dis_call =
    puts 'TO: '; dis_fun_lookup (refx dis_pc) dp;
    puts ' ARGS: '; putd (dis_call_arity (refx dis_pc)); dis_pc : dis_pc + 2

fun dis_op val =
    dis_pc : (dis_pc+1);
    if val = 0 then (puts 'HALT'; dis_pc : 0)   #flag to stop
    else if val = 0x01 then (puts 'PUSH  0x'; putx (dis_value 4))
    else if val = 0x25 then (puts 'PUSHW 0x'; putx (dis_value 2))
    else if val = 0x26 then (puts 'PUSHB 0x'; putx (dis_value 1))
    else if val = 0x02 then  puts 'POP'
    else if val = 0x03 then (puts 'PUSH_STRING "' ; dis_string ; puts '"')
    else if val = 0x04 then (puts 'GLOBAL_FETCH ' ; putd (dis_value 2))
    else if val = 0x05 then (puts 'GLOBAL_STORE ' ; putd (dis_value 2))
    else if val = 0x06 then (puts 'LOCAL_FETCH '  ; putd (dis_value 1))
    else if val = 0x07 then (puts 'TCALL '        ; dis_call)
    else if val = 0x08 then (puts 'CALL '         ; dis_call)
    else if val = 0x27 then (puts 'CCALL '        ; dis_call)
    else if val = 0x09 then (puts 'RETURN'        ; dis_pc : 0)
    else if val = 0x0a then (puts 'BRANCH '       ; putd (dis_value 2))
    else if val = 0x0b then (puts 'JUMP '         ; putd (dis_value 2))
    else if val = 0x0c then  puts 'ADD'
    else if val = 0x0d then  puts 'SUB'
    else if val = 0x0e then  puts 'MUL'
    else if val = 0x0f then  puts 'DIV'
    else if val = 0x10 then  puts 'MOD'
    else if val = 0x11 then  puts 'UMUL'
    else if val = 0x12 then  puts 'UDIV'
    else if val = 0x13 then  puts 'UMOD'
    else if val = 0x14 then  puts 'NEGATE'
    else if val = 0x15 then  puts 'EQ'
    else if val = 0x16 then  puts 'LT'
    else if val = 0x17 then  puts 'ULT'
    else if val = 0x18 then  puts 'AND'
    else if val = 0x19 then  puts 'OR'
    else if val = 0x1a then  puts 'XOR'
    else if val = 0x1b then  puts 'SLA'
    else if val = 0x1c then  puts 'SRA'
    else if val = 0x1d then  puts 'SRL'
    else if val = 0x1e then  puts 'GETC'
    else if val = 0x1f then  puts 'PUTC' 
    else if val = 0x20 then  puts 'REFB' 
    else if val = 0x21 then  puts 'REFV'
    else if val = 0x22 then  puts 'SETV'
    else if val = 0x23 then  puts 'LOCAL_FETCH_0'
    else if val = 0x24 then  puts 'LOCAL_FETCH_1'
    else if val = 0x28 then  puts 'REFX'
    else if val = 0x29 then  puts 'SETX'
    else if val = 0x2a then  puts 'SETX'
    else (puts 'UNKNOWN: 0x' ; putx val; dis_pc : 0)


fun dis_help =
    if dis_pc = 0 then cr
    else ( dis_op *dis_pc; cr; dis_help)

fun dasm str =
    dis_pc : find str;
    cr; puts str ; puts ' arity: '; putd *dis_pc; 
    dis_pc : (dis_pc+1); puts ' at: 0x'; putx (dis_pc - c0);
    cr; dis_help

# Nice little header
cr;cr;puts 'Disassembler Loaded';cr;
putd (dp-cp); puts ' bytes ('; 
putd (perc_remaining); puts '%) remaining.'; cr

