.text
.global _start

_start:
    # Bandera de direccion en 0: memcpy/memset usan instrucciones de string, que
    # con DF=1 caminan hacia atras. El kernel ya arranca los procesos con
    # rflags=0x202, pero dejarlo explicito ata la precondicion al mismo lado que
    # la depende.
    cld
    # Alinear el stack a 16 bytes antes del primer call: el ABI System V exige
    # rsp alineado a 16 en cada call (rsp % 16 == 0), y SSE (doubles de Haxe en
    # el subsistema nativo) puede emitir movaps que fallan con #GP si no lo esta.
    andq $-16, %rsp
    # El kernel deja un canary nuevo en rdx al crear el proceso. rdx no forma
    # parte del ABI de entrada a main, asi que crt0 lo instala antes de la
    # primera llamada protegida. El byte bajo nunca es cero (ABI canary).
    movq %rdx, __stack_chk_guard(%rip)
    # rcx trae la ruta del interprete (PT_INTERP) que el loader copio al stack, o 0
    # si la imagen es estatica. r8 trae la direccion de la cabecera ELF de la imagen:
    # el kernel la calculo al mapearla y es el dato exacto. Antes el cargador lo
    # adivinaba retrocediendo pagina a pagina desde una funcion suya, lo que supone
    # que toda pagina del camino esta mapeada, y un enlazador puede dejar un hueco
    # entre dos PT_LOAD.
    #
    # Los dos se PASAN a sx_start_dynamic en vez de escribirse en un global. No es
    # una preferencia de estilo: cuando libc sea una libreria, un global escrito
    # aca estaria en una imagen que todavia no esta cargada, y no habria pagina que
    # escribir. Los registros no tienen ese problema.
    #
    # Correr el interprete antes de main. El hook vive en C, no aca: comprobar si
    # un simbolo debil esta definido exige mirar su entrada en el GOT, y en
    # ensamblador habria que hacer movq simbolo(%rip) -- que LEE la memoria de esa
    # direccion. Con el simbolo debil sin definir esa direccion es 0 y el proceso
    # muere leyendo la pagina cero antes de llegar a main.
    #
    # %rdi y %rsi traen argc y argv desde el kernel, y no hay donde copiarlos:
    # una llamada puede usar cualquiera de los dos. Hay que guardarlos en el
    # stack y restaurarlos antes de entrar a main. Se hace antes de repurposar
    # %rdi y %rsi como argumentos, y los dos push dejan rsp alineado a 16 igual que
    # estaba: main exige rsp % 16 == 0 en el call, y main es justamente a donde se
    # llega.
    pushq %rdi
    pushq %rsi
    movq %rcx, %rdi
    movq %r8, %rsi
    call sx_start_dynamic
    popq %rsi
    popq %rdi
    call main
    movq %rax, %rdi
    movq $7, %rax
    int $0x80
1:
    hlt
    jmp 1b
